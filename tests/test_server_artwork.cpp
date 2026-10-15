// Copyright 2026 Sendspin Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file test_server_artwork.cpp
/// @brief The artwork and color roles end to end: the raw client checks the exact messages, the
/// parts and their pacing, cancels, the order behind control messages and a configuration change
/// mid-transfer; in-process sendspin-cpp clients show what a client library decodes and displays.

#include "artwork_stream.h"
#include "colors.h"
#include "image_codec.h"
#include "peer.h"
#include "raw_client.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>
#include <stb_image_write.h>

#include <sendspin/artwork_role.h>
#include <sendspin/client.h>
#include <sendspin/color_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ArtworkSource;
using sendspin::server::Colors;
using sendspin::server::DecodedImage;
using sendspin::server::GroupId;
using sendspin::server::RgbColor;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::test::AlwaysReady;
using sendspin::server::test::Bytes;
using sendspin::server::test::client_at;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;

// ==============================================================================
// Test images
// ==============================================================================

void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// A width by height PNG: one color, or seeded noise when seed is set, which compresses badly so
/// that its encodings take many parts
std::vector<uint8_t> png(int width, int height, std::array<uint8_t, 3> rgb,
                         std::optional<uint32_t> seed = std::nullopt) {
    std::mt19937 random(seed.value_or(0));
    std::vector<uint8_t> pixels;
    for (int i = 0; i < width * height; ++i) {
        for (const uint8_t c : rgb) {
            pixels.push_back(seed ? static_cast<uint8_t>(random()) : c);
        }
    }
    std::vector<uint8_t> out;
    stbi_write_png_to_func(&append, &out, width, height, 3, pixels.data(), width * 3);
    return out;
}

std::array<uint8_t, 3> pixel(const DecodedImage& image, uint32_t x, uint32_t y) {
    const size_t at = (size_t{y} * image.width + x) * 4;
    return {image.rgba()[at], image.rgba()[at + 1], image.rgba()[at + 2]};
}

bool near(std::array<uint8_t, 3> got, std::array<uint8_t, 3> want) {
    constexpr int TOLERANCE = 24;
    for (size_t c = 0; c < 3; ++c) {
        if (std::abs(got[c] - want[c]) > TOLERANCE) {
            return false;
        }
    }
    return true;
}

// ==============================================================================
// Raw client
// ==============================================================================

const std::string HELLO =
    R"({"type":"client/hello","payload":{"name":"Frame","supported_roles":["artwork@v1",)"
    R"("color@v1"],"supported_pair_methods":{"pairing_psk":{}},"unpaired_access":)"
    R"({"enabled":true}}})";

std::string state(bool available, const std::string& channels) {
    return R"({"type":"client/state","payload":{"available":)" +
           std::string(available ? "true" : "false") + R"(,"artwork":{"channels":)" + channels +
           "}}}";
}

const std::string PNG_200_100 = R"([{"source":"album","format":"png","width":200,"height":100}])";

/// One message the server sent: JSON, or an artwork message
struct Message {
    Bytes plaintext;

    bool json() const {
        return this->plaintext[0] == 0;
    }
    std::string text() const {
        return {this->plaintext.begin() + 1, this->plaintext.end()};
    }
    std::string type() const {
        JsonDocument doc;
        deserializeJson(doc, this->text());
        return doc["type"].as<std::string>();
    }
    int channel() const {
        return this->plaintext[0] - 8;
    }
    bool announce() const {
        return !this->json() && this->plaintext[1] == 2;
    }
    bool cancel() const {
        return !this->json() && this->plaintext[1] == 1;
    }
    bool part() const {
        return !this->json() && this->plaintext[1] == 0;
    }
    int64_t timestamp() const {
        uint64_t ts = 0;
        for (size_t i = 2; i < 10; ++i) {
            ts = (ts << 8) | this->plaintext[i];
        }
        return static_cast<int64_t>(ts);
    }
    uint32_t total_size() const {
        uint32_t size = 0;
        for (size_t i = 10; i < 14; ++i) {
            size = (size << 8) | this->plaintext[i];
        }
        return size;
    }
};

/// Reads what the server sends one message at a time, keeping the JSON messages it skips
class Reader {
public:
    explicit Reader(RawClient& client) : client_(client) {}

    Message next() {
        std::optional<Bytes> plaintext = this->client_.receive_plaintext();
        EXPECT_TRUE(plaintext) << "the connection closed";
        return {plaintext ? *plaintext : Bytes{0}};
    }

    /// The next artwork message, keeping the JSON before it
    Message next_artwork() {
        for (;;) {
            Message message = this->next();
            if (!message.json() || message.plaintext.size() <= 1) {
                return message;
            }
            this->json.push_back(message.text());
        }
    }

    /// The parts of the transfer whose announce was read, concatenated
    Bytes parts(uint32_t total) {
        Bytes image;
        while (image.size() < total) {
            const Message part = this->next_artwork();
            EXPECT_TRUE(part.part());
            if (!part.part()) {
                return image;
            }
            image.insert(image.end(), part.plaintext.begin() + 2, part.plaintext.end());
        }
        return image;
    }

    /// Whether a JSON message kept holds text
    bool saw(const std::string& text) const {
        return std::any_of(this->json.begin(), this->json.end(), [&](const std::string& json) {
            return json.find(text) != std::string::npos;
        });
    }

    std::vector<std::string> json;

private:
    RawClient& client_;
};

class RawServer {
public:
    explicit RawServer(size_t max_message_bytes = 4096) {
        ServerConfig config = sendspin::server::test::local_config();
        config.approve_all_clients = true;
        config.max_message_bytes = max_message_bytes;
        config.trace = [this](const sendspin::server::TracedMessage& message) {
            this->recorder.trace(message);
            // Outbound artwork parts, for their stamps
            if (message.direction == sendspin::server::TraceDirection::OUTBOUND && message.binary &&
                message.plaintext.size() > 2 && message.plaintext[0] == 8 &&
                message.plaintext[1] == 0) {
                const std::lock_guard<std::mutex> lock(this->mutex);
                this->part_stamps.push_back(message.stamp_us);
            }
        };
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
        this->group = *this->server.default_group();
    }

    /// Connects client and reads through the stream/start its client/state asks for
    void connect(RawClient& client, Reader& reader, const std::string& channels) {
        ASSERT_TRUE(this->server.connect_url(client.url()));
        ASSERT_NO_FATAL_FAILURE(client.activate(HELLO));
        client.send_json(state(true, channels));
        for (;;) {
            const Message message = reader.next();
            ASSERT_TRUE(message.json()) << "artwork before stream/start";
            if (message.type() == "stream/start") {
                this->stream_start = message.text();
                return;
            }
        }
    }

    std::vector<int64_t> stamps() {
        const std::lock_guard<std::mutex> lock(this->mutex);
        return this->part_stamps;
    }

    MemoryProvider provider;
    Recorder recorder;
    // The trace writes these on the dispatch thread until the server stops, so they outlive it
    std::mutex mutex;
    std::vector<int64_t> part_stamps;
    SendspinServer server;
    GroupId group;
    std::string stream_start;
};

// spec roles/artwork/v1.md: the stream/start echoes the client/state channels; then the current
// image, none, as an empty announce ("Late join"); a host's image as an announce of its size at
// its timestamp and parts of at most max_message_bytes, paced (ARTWORK_PART_INTERVAL_US apart,
// by the stamps taken before encryption), whose bytes are a PNG of exactly the channel's size
// with the image letterboxed in black; its palette reaches the color role
TEST(ServerArtworkWire, SendsExactMessagesAndPacedParts) {
    RawServer s(4096);
    RawClient client;
    Reader reader(client);
    ASSERT_NO_FATAL_FAILURE(s.connect(client, reader, PNG_200_100));
    JsonDocument start;
    deserializeJson(start, s.stream_start);
    std::string channels;
    serializeJson(start["payload"]["artwork"], channels);
    EXPECT_EQ(channels, R"({"channels":[{"source":"album","format":"png","width":200,)"
                        R"("height":100}]})");
    EXPECT_TRUE(start["payload"]["server_transmitted"].is<int64_t>());
    const Message empty = reader.next_artwork();
    ASSERT_TRUE(empty.announce());
    EXPECT_EQ(empty.plaintext.size(), 14U);
    EXPECT_EQ(empty.channel(), 0);
    EXPECT_EQ(empty.total_size(), 0U);

    const int64_t at = s.server.now_us();
    ASSERT_TRUE(
        s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(300, 300, {0, 0, 0}, 7), at));
    const Message announce = reader.next_artwork();
    ASSERT_TRUE(announce.announce());
    EXPECT_EQ(announce.timestamp(), at);
    const Bytes image = reader.parts(announce.total_size());
    ASSERT_EQ(image.size(), announce.total_size());
    const std::optional<DecodedImage> decoded = sendspin::server::decode_image(image);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->width, 200U);
    EXPECT_EQ(decoded->height, 100U);
    // A square image in a 200 by 100 channel: 100 by 100 at x 50, black either side
    EXPECT_EQ(pixel(*decoded, 0, 50), (std::array<uint8_t, 3>{0, 0, 0}));
    EXPECT_EQ(pixel(*decoded, 49, 99), (std::array<uint8_t, 3>{0, 0, 0}));
    EXPECT_EQ(pixel(*decoded, 150, 0), (std::array<uint8_t, 3>{0, 0, 0}));
    EXPECT_EQ(pixel(*decoded, 199, 50), (std::array<uint8_t, 3>{0, 0, 0}));
    EXPECT_NE(pixel(*decoded, 100, 50), (std::array<uint8_t, 3>{0, 0, 0}));
    // The trace reaches the dispatch thread after the client can have read every part, so wait
    // for it, with no timeout: a part never traced hangs into the suite watchdog
    const size_t parts = (image.size() + 4093) / 4094;
    std::vector<int64_t> stamps = s.stamps();
    while (stamps.size() < parts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        stamps = s.stamps();
    }
    ASSERT_EQ(stamps.size(), parts);
    ASSERT_GE(stamps.size(), 3U);
    for (size_t i = 1; i < stamps.size(); ++i) {
        EXPECT_GE(stamps[i] - stamps[i - 1], sendspin::server::ARTWORK_PART_INTERVAL_US);
    }
    // The album image's palette reaches the color role, at the image's time
    while (!reader.saw(R"("primary")")) {
        reader.json.push_back(reader.next().text());
    }
    EXPECT_TRUE(reader.saw(R"({"type":"server/state","payload":{"color":{"timestamp":)" +
                           std::to_string(at)));
}

// spec roles/artwork/v1.md "Artwork (Binary)": a transfer in flight is cancelled before another
// image is announced. The client reads nothing while the host replaces the image, so the image
// in flight, far larger than the socket buffers, cannot finish before the server decides again.
TEST(ServerArtworkWire, CancelsATransferInFlightForANewImage) {
    RawServer s(4096);
    RawClient client;
    client.fix_receive_buffer(4096);
    Reader reader(client);
    ASSERT_NO_FATAL_FAILURE(s.connect(
        client, reader, R"([{"source":"album","format":"png","width":300,"height":300}])"));
    ASSERT_TRUE(reader.next_artwork().announce());
    ASSERT_TRUE(s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(300, 300, {0, 0, 0}, 1),
                                     s.server.now_us()));
    const Message first = reader.next_artwork();
    ASSERT_TRUE(first.announce());
    ASSERT_GT(first.total_size(), 200000U);
    ASSERT_TRUE(reader.next_artwork().part());
    const int64_t at = s.server.now_us();
    ASSERT_TRUE(s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(10, 10, {200, 0, 0}), at));
    size_t received = 0;
    Message message = reader.next_artwork();
    while (message.part()) {
        received += message.plaintext.size() - 2;
        message = reader.next_artwork();
    }
    EXPECT_LT(received, first.total_size());
    ASSERT_TRUE(message.cancel());
    EXPECT_EQ(message.plaintext, (Bytes{8, 1}));
    const Message second = reader.next_artwork();
    ASSERT_TRUE(second.announce());
    EXPECT_EQ(second.timestamp(), at);
    const std::optional<DecodedImage> decoded =
        sendspin::server::decode_image(reader.parts(second.total_size()));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->width, 300U);
    EXPECT_TRUE(near(pixel(*decoded, 150, 150), {200, 0, 0}));
}

// Control messages go ahead of the artwork queue; a server/state queued while a transfer is in
// flight arrives before the transfer ends
TEST(ServerArtworkWire, SendsControlMessagesAheadOfTheArtworkQueue) {
    RawServer s(4096);
    RawClient client;
    client.fix_receive_buffer(4096);
    Reader reader(client);
    ASSERT_NO_FATAL_FAILURE(s.connect(
        client, reader, R"([{"source":"album","format":"png","width":300,"height":300}])"));
    ASSERT_TRUE(reader.next_artwork().announce());
    ASSERT_TRUE(s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(300, 300, {0, 0, 0}, 3),
                                     s.server.now_us()));
    const Message announce = reader.next_artwork();
    ASSERT_TRUE(announce.announce());
    ASSERT_TRUE(reader.next_artwork().part());
    reader.json.clear();
    Colors colors;
    colors.primary = RgbColor{1, 2, 3};
    ASSERT_TRUE(s.server.set_colors(s.group, colors, s.server.now_us()));
    size_t received = 0;
    for (;;) {
        const Message message = reader.next_artwork();
        if (reader.saw(R"("primary":[1,2,3])")) {
            break;
        }
        ASSERT_TRUE(message.part());
        received += message.plaintext.size() - 2;
        ASSERT_LT(received, announce.total_size()) << "the transfer ended first";
    }
}

// spec roles/artwork/v1.md "client/state artwork object": a size change mid-transfer cancels the
// transfer, sends a stream/start with the new configuration and the image again at the new size
TEST(ServerArtworkWire, ReconfiguresMidTransfer) {
    RawServer s(4096);
    RawClient client;
    client.fix_receive_buffer(4096);
    Reader reader(client);
    ASSERT_NO_FATAL_FAILURE(s.connect(
        client, reader, R"([{"source":"album","format":"png","width":300,"height":300}])"));
    ASSERT_TRUE(reader.next_artwork().announce());
    ASSERT_TRUE(s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(300, 300, {0, 0, 0}, 4),
                                     s.server.now_us()));
    const Message announce = reader.next_artwork();
    ASSERT_TRUE(announce.announce());
    ASSERT_TRUE(reader.next_artwork().part());
    client.send_json(state(true, R"([{"source":"album","format":"jpeg","width":50,"height":40}])"));
    Message message = reader.next_artwork();
    while (message.part()) {
        message = reader.next_artwork();
    }
    ASSERT_TRUE(message.cancel());
    reader.json.clear();
    const Message again = reader.next_artwork();
    // The stream/start, a JSON message between the cancel and the next artwork message
    JsonDocument start;
    for (const std::string& json : reader.json) {
        JsonDocument doc;
        deserializeJson(doc, json);
        if (doc["type"] == "stream/start") {
            start = doc;
        }
    }
    EXPECT_EQ(start["payload"]["artwork"]["channels"][0]["width"], 50);
    EXPECT_EQ(start["payload"]["artwork"]["channels"][0]["format"], "jpeg");
    ASSERT_TRUE(again.announce());
    EXPECT_EQ(again.timestamp(), announce.timestamp());
    const std::optional<DecodedImage> decoded =
        sendspin::server::decode_image(reader.parts(again.total_size()));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->width, 50U);
    EXPECT_EQ(decoded->height, 40U);
}

// spec roles/artwork/v1.md "client/state artwork object": more than 4 channels is a protocol
// error, which closes the connection; an object invalid otherwise is ignored, the channels
// declared before kept
TEST(ServerArtworkWire, IgnoresAnInvalidObjectAndClosesOnMoreThanFourChannels) {
    RawServer s;
    RawClient client;
    Reader reader(client);
    ASSERT_NO_FATAL_FAILURE(s.connect(client, reader, PNG_200_100));
    ASSERT_TRUE(reader.next_artwork().announce());
    client.send_json(state(true, R"([{"source":"album","format":"bmp","width":1,"height":1}])"));
    client.send_json(state(true, R"([])"));
    ASSERT_TRUE(s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(20, 20, {0, 0, 0}),
                                     s.server.now_us()));
    const Message announce = reader.next_artwork();
    ASSERT_TRUE(announce.announce());
    const std::optional<DecodedImage> decoded =
        sendspin::server::decode_image(reader.parts(announce.total_size()));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->width, 200U);
    EXPECT_EQ(decoded->height, 100U);
    EXPECT_FALSE(reader.saw("stream/start"));
    const std::string channel = R"({"source":"none"})";
    client.send_json(state(
        true, "[" + channel + "," + channel + "," + channel + "," + channel + "," + channel + "]"));
    client.read_until_closed();
}

// messaging.md "server/activate": the artwork stream/end goes before the activation that removes
// the role, a transfer in flight cancelled first (spec roles/artwork/v1.md "Artwork (Binary)")
TEST(ServerArtworkWire, EndsTheStreamBeforeTheActivationThatRemovesIt) {
    RawServer s(4096);
    RawClient client;
    client.fix_receive_buffer(4096);
    Reader reader(client);
    ASSERT_NO_FATAL_FAILURE(s.connect(
        client, reader, R"([{"source":"album","format":"png","width":300,"height":300}])"));
    ASSERT_TRUE(reader.next_artwork().announce());
    ASSERT_TRUE(s.server.set_artwork(s.group, ArtworkSource::ALBUM, png(300, 300, {0, 0, 0}, 5),
                                     s.server.now_us()));
    ASSERT_TRUE(reader.next_artwork().announce());
    ASSERT_TRUE(reader.next_artwork().part());
    ASSERT_TRUE(s.server.revoke_approval(client.noise.identity().peer_id()));
    Message message = reader.next_artwork();
    while (message.part()) {
        message = reader.next_artwork();
    }
    ASSERT_TRUE(message.cancel());
    reader.json.clear();
    while (!reader.saw("server/activate")) {
        const Message next = reader.next();
        ASSERT_TRUE(next.json());
        reader.json.push_back(next.text());
    }
    ASSERT_GE(reader.json.size(), 2U);
    EXPECT_EQ(reader.json[reader.json.size() - 2],
              R"({"type":"stream/end","payload":{"roles":["artwork"]}})");
}

// ==============================================================================
// In-process sendspin-cpp clients
// ==============================================================================

/// What a sendspin-cpp client's artwork and color roles report: images on its decode thread,
/// the rest on its main loop, the test's thread
class Events : public sendspin::ArtworkRoleListener, public sendspin::ColorRoleListener {
public:
    void on_image_decode(uint8_t slot, const uint8_t* data, size_t length,
                         sendspin::SendspinImageFormat /*format*/) override {
        std::optional<DecodedImage> decoded =
            sendspin::server::decode_image(std::span<const uint8_t>(data, length));
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->decoded_[slot] =
            decoded ? std::make_shared<const DecodedImage>(std::move(*decoded)) : nullptr;
    }

    void on_image_display(uint8_t slot, uint32_t /*lateness_ms*/) override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->shown[slot] = this->decoded_[slot];
        ++this->displays[slot];
    }

    void on_image_clear(uint8_t slot) override {
        this->shown[slot].reset();
        ++this->clears[slot];
    }

    void on_color(const sendspin::ServerColorStateObject& color) override {
        this->colors.push_back(color);
    }

    /// Whether slot shows an image of width by height whose centre is near rgb
    bool shows(uint8_t slot, uint32_t width, uint32_t height, std::array<uint8_t, 3> rgb) const {
        const std::shared_ptr<const DecodedImage>& image = this->shown[slot];
        return image && image->width == width && image->height == height &&
               near(pixel(*image, width / 2, height / 2), rgb);
    }

    std::array<std::shared_ptr<const DecodedImage>, 2> shown;
    std::array<int, 2> displays{};
    std::array<int, 2> clears{};
    std::vector<sendspin::ServerColorStateObject> colors;

private:
    std::mutex mutex_;
    std::array<std::shared_ptr<const DecodedImage>, 2> decoded_;
};

/// A sendspin-cpp client with the artwork role (an album JPEG channel of 64 by 64 and an artist
/// PNG channel of 40 by 20) and the color role, listening for the server to dial
class Peer {
public:
    Peer()
        : port_(sendspin::server::test::free_port()),
          client_(sendspin::server::test::peer_config("Frame", this->port_)) {
        sendspin::ArtworkRoleConfig config;
        sendspin::ImageSlotPreference album;
        album.source = sendspin::SendspinImageSource::ALBUM;
        album.format = sendspin::SendspinImageFormat::JPEG;
        album.width = 64;
        album.height = 64;
        sendspin::ImageSlotPreference artist;
        artist.source = sendspin::SendspinImageSource::ARTIST;
        artist.format = sendspin::SendspinImageFormat::PNG;
        artist.width = 40;
        artist.height = 20;
        config.preferred_formats = {album, artist};
        this->client_.add_artwork(config).set_listener(&this->events);
        this->client_.add_color().set_listener(&this->events);
        this->client_.set_network_provider(&this->network_);
        this->client_.set_unpaired_access_enabled(true);
        EXPECT_TRUE(this->client_.start());
    }

    ~Peer() {
        this->client_.stop();
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    /// Runs the client's main loop on this thread until done() holds, with no timeout
    template <typename Done>
    void run_until(Done done) {
        sendspin::server::test::run_until(this->client_, done);
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    Events events;

private:
    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
};

constexpr std::array<uint8_t, 3> RED{255, 0, 0};
constexpr std::array<uint8_t, 3> BLUE{0, 0, 255};
constexpr std::array<uint8_t, 3> GREEN{0, 255, 0};
constexpr std::array<uint8_t, 3> YELLOW{255, 255, 0};
constexpr std::array<uint8_t, 3> BLACK{0, 0, 0};

// spec roles/artwork/v1.md with a sendspin-cpp client: each channel decodes an image of exactly
// its size, a wide one letterboxed in black; a track change replaces it, a clear empties the
// channel; unavailability ends the stream, which clears every channel, and availability brings the
// current image back; a move shows the other group's images
TEST(ServerArtworkInProcess, ImagesReachASendspinCppClient) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    Peer peer;
    ASSERT_TRUE(server.connect_url(peer.url()));
    Events& events = peer.events;

    // Wide red art: 64 by 32 in the middle of the album channel, black above and below
    ASSERT_TRUE(
        server.set_artwork(group, ArtworkSource::ALBUM, png(200, 100, RED), server.now_us()));
    ASSERT_TRUE(
        server.set_artwork(group, ArtworkSource::ARTIST, png(10, 10, BLUE), server.now_us()));
    peer.run_until([&] { return events.shows(0, 64, 64, RED) && events.shows(1, 40, 20, BLUE); });
    EXPECT_TRUE(near(pixel(*events.shown[0], 32, 4), BLACK));
    EXPECT_TRUE(near(pixel(*events.shown[0], 32, 59), BLACK));
    // The artist image is padded left and right
    EXPECT_TRUE(near(pixel(*events.shown[1], 2, 10), BLACK));
    EXPECT_TRUE(near(pixel(*events.shown[1], 37, 10), BLACK));

    // A track change
    ASSERT_TRUE(
        server.set_artwork(group, ArtworkSource::ALBUM, png(50, 50, GREEN), server.now_us()));
    peer.run_until([&] { return events.shows(0, 64, 64, GREEN); });
    // Cleared, as a host does when playback stops
    const int clears = events.clears[0];
    ASSERT_TRUE(server.set_artwork(group, ArtworkSource::ALBUM, {}, server.now_us()));
    peer.run_until([&] { return events.clears[0] > clears && !events.shown[0]; });
    EXPECT_TRUE(events.shows(1, 40, 20, BLUE));

    // Unavailable: the stream ends and every channel clears; available again: the images return
    ASSERT_TRUE(
        server.set_artwork(group, ArtworkSource::ALBUM, png(50, 50, GREEN), server.now_us()));
    peer.run_until([&] { return events.shows(0, 64, 64, GREEN); });
    const int displays = events.displays[0];
    peer.client().set_available(false);
    peer.run_until([&] { return !events.shown[0] && !events.shown[1]; });
    peer.client().set_available(true);
    peer.run_until([&] {
        return events.displays[0] > displays && events.shows(0, 64, 64, GREEN) &&
               events.shows(1, 40, 20, BLUE);
    });

    // A move into a group with no images clears the channels; its images then show
    const std::string id = client_at(server, peer.url());
    const std::optional<GroupId> other = server.create_group("Other", id);
    ASSERT_TRUE(other);
    peer.run_until([&] { return !events.shown[0] && !events.shown[1]; });
    recorder.wait_for("group " + std::to_string(other->value) + " stopped");
    ASSERT_TRUE(
        server.set_artwork(*other, ArtworkSource::ALBUM, png(20, 20, YELLOW), server.now_us()));
    peer.run_until([&] { return events.shows(0, 64, 64, YELLOW); });
    // And back: the first group's images again
    ASSERT_TRUE(server.move_client(id, group));
    peer.run_until([&] { return events.shows(0, 64, 64, GREEN) && events.shows(1, 40, 20, BLUE); });
}

// spec roles/color/v1.md with a sendspin-cpp client: the album image's palette, then the host's
// colors scheduled ahead, which the client applies at their timestamp, and their contrast
TEST(ServerArtworkInProcess, ColorsReachASendspinCppClientOnTheTimeline) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    Peer peer;
    ASSERT_TRUE(server.connect_url(peer.url()));
    Events& events = peer.events;
    // Joined with no colors: an empty state
    peer.run_until([&] { return !events.colors.empty(); });
    EXPECT_FALSE(events.colors.back().primary);

    // An album image's palette: its dominant color, the 5-bit bucket centre of pure red
    const int64_t art_at = server.now_us();
    ASSERT_TRUE(server.set_artwork(group, ArtworkSource::ALBUM, png(32, 32, RED), art_at));
    peer.run_until([&] { return events.colors.back().primary.has_value(); });
    EXPECT_EQ(events.colors.back().timestamp, art_at);
    EXPECT_EQ(*events.colors.back().primary, (sendspin::RgbColor{252, 4, 4}));
    ASSERT_TRUE(events.colors.back().background_dark);

    // The host's colors, scheduled ahead: applied at their timestamp, with the contrast the spec
    // requires: a white on_light is moved toward black until it has 4.5:1 with white
    Colors colors;
    colors.primary = RgbColor{10, 20, 30};
    colors.on_light = RgbColor{255, 255, 255};
    constexpr int64_t AHEAD_US = 300000;
    const int64_t at = server.now_us() + AHEAD_US;
    ASSERT_TRUE(server.set_colors(group, colors, at));
    const size_t before = events.colors.size();
    peer.run_until([&] { return events.colors.size() > before; });
    const sendspin::ServerColorStateObject& applied = events.colors.back();
    EXPECT_EQ(applied.timestamp, at);
    EXPECT_EQ(*applied.primary, (sendspin::RgbColor{10, 20, 30}));
    ASSERT_TRUE(applied.on_light);
    EXPECT_GE(sendspin::server::contrast_ratio(
                  {(*applied.on_light)[0], (*applied.on_light)[1], (*applied.on_light)[2]},
                  {255, 255, 255}),
              4.5);
    EXPECT_FALSE(applied.background_dark);
}

// messaging.md "client/leave": a client that leaves its solo group stops it and its streams end,
// the artwork stream with them; the group's next run starts them again, the images returning
TEST(ServerArtworkInProcess, ImagesReturnWhenTheGroupALeaverStoppedPlaysAgain) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    Peer peer;
    ASSERT_TRUE(server.connect_url(peer.url()));
    Events& events = peer.events;
    ASSERT_TRUE(
        server.set_artwork(group, ArtworkSource::ALBUM, png(50, 50, GREEN), server.now_us()));
    peer.run_until([&] { return events.shows(0, 64, 64, GREEN); });

    peer.client().leave();
    peer.run_until([&] { return !events.shown[0]; });
    const int displays = events.displays[0];

    // The host plays to the group again: a new run, and the image with it
    sendspin::server::InputOptions options;
    options.format = {48000, 2, sendspin::server::SampleFormat::S16};
    const std::shared_ptr<sendspin::server::GroupInput> input =
        server.open_input(group, options).input;
    ASSERT_TRUE(input);
    constexpr size_t FRAMES = 960;
    const std::vector<uint8_t> silence(FRAMES * 4, 0);
    peer.run_until([&] {
        input->write(silence.data(), FRAMES);
        return events.displays[0] > displays && events.shows(0, 64, 64, GREEN);
    });
    input->stop();
}

}  // namespace
