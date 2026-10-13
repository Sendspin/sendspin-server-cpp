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

#include "messages.h"

#include "crypto/base64url.h"
#include "crypto/crypto.h"
#include "fragmentation.h"
#include <ArduinoJson.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace sendspin::server {

namespace {

/// An ArduinoJson allocator that wipes every block it frees, so a pairing secret parsed or
/// redacted leaves no copy in freed memory. Each block is prefixed with its size.
class WipingAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        auto* block = static_cast<size_t*>(std::malloc(HEADER + size));
        if (block == nullptr) {
            return nullptr;
        }
        *block = size;
        return reinterpret_cast<uint8_t*>(block) + HEADER;
    }

    void deallocate(void* pointer) override {
        if (pointer == nullptr) {
            return;
        }
        auto* block = reinterpret_cast<size_t*>(static_cast<uint8_t*>(pointer) - HEADER);
        secure_zero(pointer, *block);
        std::free(block);
    }

    void* reallocate(void* pointer, size_t new_size) override {
        void* moved = this->allocate(new_size);
        if (moved != nullptr && pointer != nullptr) {
            const size_t old_size =
                *reinterpret_cast<size_t*>(static_cast<uint8_t*>(pointer) - HEADER);
            std::memcpy(moved, pointer, std::min(old_size, new_size));
            this->deallocate(pointer);
        }
        return moved;
    }

    static WipingAllocator* instance() {
        static WipingAllocator allocator;
        return &allocator;
    }

private:
    /// Keeps the block aligned as malloc's
    static constexpr size_t HEADER = alignof(std::max_align_t);
};

// ==============================================================================
// Field readers. A reader returns false (or nullopt) for a present field of the wrong type or
// range, so the caller rejects the enclosing object.
// ==============================================================================

std::optional<uint32_t> read_positive_u32(JsonVariantConst value) {
    if (!value.is<uint32_t>() || value.as<uint32_t>() == 0) {
        return std::nullopt;
    }
    return value.as<uint32_t>();
}

bool read_optional_string(JsonVariantConst value, std::optional<std::string>& out) {
    if (value.isNull()) {
        return true;
    }
    if (!value.is<const char*>()) {
        return false;
    }
    out = value.as<std::string>();
    return true;
}

struct FlagName {
    std::string_view name;
    uint8_t bit;
};

/// The bits of the recognized strings in an array of strings; unrecognized strings add none
template <size_t N>
std::optional<uint8_t> read_flags(JsonVariantConst value, const std::array<FlagName, N>& names) {
    if (!value.is<JsonArrayConst>()) {
        return std::nullopt;
    }
    uint8_t flags = 0;
    for (const JsonVariantConst entry : value.as<JsonArrayConst>()) {
        if (!entry.is<const char*>()) {
            return std::nullopt;
        }
        const std::string_view text = entry.as<const char*>();
        for (const FlagName& flag : names) {
            if (flag.name == text) {
                flags |= flag.bit;
            }
        }
    }
    return flags;
}

constexpr std::array<FlagName, 3> LOCATION_NAMES{{
    {"device", pair_location::DEVICE},
    {"leaflet", pair_location::LEAFLET},
    {"operator", pair_location::OPERATOR},
}};

constexpr std::array<FlagName, 2> CHANNEL_NAMES{{
    {"display", pair_code::DISPLAY},
    {"speaker", pair_code::SPEAKER},
}};

constexpr std::array<FlagName, 2> FORMAT_NAMES{{
    {"digits", pair_code::DIGITS},
    {"qr_code", pair_code::QR_CODE},
}};

// The role versions this specification defines (messaging.md "client/hello")
constexpr std::array<std::string_view, 7> KNOWN_ROLES{
    "player@v1",  "source@v1",     "controller@v1", "metadata@v1",
    "artwork@v1", "visualizer@v1", "color@v1",
};

constexpr std::array<std::pair<std::string_view, GoodbyeReason>, 8> GOODBYE_REASONS{{
    {"another_server", GoodbyeReason::ANOTHER_SERVER},
    {"shutdown", GoodbyeReason::SHUTDOWN},
    {"restart", GoodbyeReason::RESTART},
    {"user_request", GoodbyeReason::USER_REQUEST},
    {"unauthorized", GoodbyeReason::UNAUTHORIZED},
    {"pairing_required", GoodbyeReason::PAIRING_REQUIRED},
    {"concurrent_attempt", GoodbyeReason::CONCURRENT_ATTEMPT},
    {"unpaired", GoodbyeReason::UNPAIRED},
}};

// README "Detecting Outdated Servers": a role or a version starting with `_` is
// application-specific, never a sign of a newer revision
bool is_application_specific(std::string_view role) {
    const size_t at = role.find('@');
    return role.starts_with('_') ||
           (at != std::string_view::npos && role.substr(at + 1).starts_with('_'));
}

bool is_listed(const std::vector<std::string>& roles, std::string_view role) {
    for (const std::string& listed : roles) {
        if (listed == role) {
            return true;
        }
    }
    return false;
}

/// The JSON, or nullopt when a value could not be stored (ArduinoJson then writes null for it)
std::optional<std::string> try_serialize(const JsonDocument& doc) {
    if (doc.overflowed()) {
        return std::nullopt;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

/// For a document of fixed-size values, which only an allocation failure can leave incomplete
std::string serialize(const JsonDocument& doc) {
    std::optional<std::string> out = try_serialize(doc);
    if (!out) {
        throw std::bad_alloc();
    }
    return std::move(*out);
}

// ==============================================================================
// client/hello parts
// ==============================================================================

std::optional<Codec> codec_from_wire(std::string_view name) {
    if (name == "opus") {
        return Codec::OPUS;
    }
    if (name == "flac") {
        return Codec::FLAC;
    }
    if (name == "pcm") {
        return Codec::PCM;
    }
    return std::nullopt;
}

std::optional<uint32_t> read_u32(JsonVariantConst value) {
    if (!value.is<uint32_t>()) {
        return std::nullopt;
    }
    return value.as<uint32_t>();
}

/// One format entry: a recognized codec with positive channels and rate and an integer bit depth.
/// nullopt for an invalid entry; known is nullopt for a valid entry with an unrecognized codec.
std::optional<std::optional<AudioFormat>> parse_format(JsonVariantConst entry) {
    const JsonVariantConst codec = entry["codec"];
    const std::optional<uint32_t> channels = read_positive_u32(entry["channels"]);
    const std::optional<uint32_t> sample_rate = read_positive_u32(entry["sample_rate"]);
    const std::optional<uint32_t> bit_depth = read_u32(entry["bit_depth"]);
    if (!entry.is<JsonObjectConst>() || !codec.is<const char*>() || !channels || !sample_rate ||
        !bit_depth) {
        return std::nullopt;
    }
    const std::optional<Codec> known = codec_from_wire(codec.as<const char*>());
    if (!known) {
        return std::optional<AudioFormat>{};
    }
    return std::optional<AudioFormat>{AudioFormat{*known, *channels, *sample_rate, *bit_depth}};
}

// roles/player/v1.md "client/hello player@v1 support object"
std::optional<PlayerSupport> parse_player_support(JsonVariantConst object) {
    const JsonVariantConst formats = object["supported_formats"];
    if (!formats.is<JsonArrayConst>() || formats.as<JsonArrayConst>().size() == 0) {
        return std::nullopt;
    }
    PlayerSupport support;
    for (const JsonVariantConst entry : formats.as<JsonArrayConst>()) {
        const std::optional<std::optional<AudioFormat>> format = parse_format(entry);
        if (!format) {
            return std::nullopt;
        }
        // A codec a newer revision may add cannot be produced, so its entry is skipped
        if (*format) {
            support.supported_formats.push_back(**format);
        }
    }
    const std::optional<uint32_t> capacity = read_positive_u32(object["buffer_capacity"]);
    if (!capacity) {
        return std::nullopt;
    }
    support.buffer_capacity = *capacity;
    return support;
}

// roles/source/v1.md "client/hello source@v1 support object"
std::optional<SourceSupport> parse_source_support(JsonVariantConst object) {
    SourceSupport support;
    const JsonVariantConst features = object["features"];
    if (features.isNull()) {
        return support;
    }
    const JsonVariantConst line_sense = features["line_sense"];
    if (!features.is<JsonObjectConst>() || (!line_sense.isNull() && !line_sense.is<bool>())) {
        return std::nullopt;
    }
    support.line_sense = line_sense.as<bool>();
    return support;
}

// roles/visualizer/v1.md "client/hello visualizer@v1 support object"
std::optional<VisualizerSupport> parse_visualizer_support(JsonVariantConst object) {
    const std::optional<uint32_t> capacity = read_positive_u32(object["buffer_capacity"]);
    if (!capacity) {
        return std::nullopt;
    }
    return VisualizerSupport{*capacity};
}

std::optional<DeviceInfo> parse_device_info(JsonVariantConst object) {
    DeviceInfo info;
    if (!object.is<JsonObjectConst>() ||
        !read_optional_string(object["product_name"], info.product_name) ||
        !read_optional_string(object["manufacturer"], info.manufacturer) ||
        !read_optional_string(object["software_version"], info.software_version) ||
        !read_optional_string(object["mac_address"], info.mac_address)) {
        return std::nullopt;
    }
    return info;
}

std::optional<PairMethodDescriptor> parse_located_method(JsonVariantConst object) {
    if (!object.is<JsonObjectConst>()) {
        return std::nullopt;
    }
    PairMethodDescriptor descriptor;
    if (!object["locations"].isNull()) {
        const std::optional<uint8_t> locations = read_flags(object["locations"], LOCATION_NAMES);
        if (!locations) {
            return std::nullopt;
        }
        descriptor.locations = *locations;
    }
    return descriptor;
}

// pairing.md "client/hello pair-method descriptor": a dynamic_pairing_code left with no
// recognized format or channel counts as an unrecognized key
std::optional<DynamicPairingCode> parse_dynamic_code(JsonVariantConst object) {
    const std::optional<uint8_t> channels = read_flags(object["out_channels"], CHANNEL_NAMES);
    const std::optional<uint8_t> formats = read_flags(object["formats"], FORMAT_NAMES);
    if (!channels || !formats || *channels == 0 || *formats == 0) {
        return std::nullopt;
    }
    return DynamicPairingCode{*channels, *formats};
}

// pairing.md "client/hello pair-method descriptor": unrecognized keys are ignored, unvalidated,
// and recorded
void parse_pair_methods(JsonObjectConst methods, ClientHello& hello) {
    for (const JsonPairConst method : methods) {
        const std::string_view key = method.key().c_str();
        if (key == "pairing_psk") {
            hello.pair_methods.pairing_psk = parse_located_method(method.value());
        } else if (key == "static_pairing_code") {
            hello.pair_methods.static_pairing_code = parse_located_method(method.value());
        } else if (key == "dynamic_pairing_code") {
            hello.pair_methods.dynamic_pairing_code = parse_dynamic_code(method.value());
            if (!hello.pair_methods.dynamic_pairing_code) {
                hello.unrecognized_identifiers.emplace_back(key);
            }
        } else {
            hello.unrecognized_identifiers.emplace_back(key);
        }
    }
}

// messaging.md "client/hello"
std::optional<ClientHello> parse_client_hello(JsonObjectConst payload, int64_t rx_us) {
    const JsonVariantConst name = payload["name"];
    const JsonVariantConst roles = payload["supported_roles"];
    const JsonVariantConst methods = payload["supported_pair_methods"];
    const JsonVariantConst unpaired = payload["unpaired_access"]["enabled"];
    if (!name.is<const char*>() || !roles.is<JsonArrayConst>() || !methods.is<JsonObjectConst>() ||
        !payload["unpaired_access"].is<JsonObjectConst>() || !unpaired.is<bool>()) {
        return std::nullopt;
    }
    ClientHello hello;
    hello.name = name.as<std::string>();
    hello.unpaired_access_enabled = unpaired.as<bool>();
    hello.rx_us = rx_us;
    for (const JsonVariantConst role : roles.as<JsonArrayConst>()) {
        if (!role.is<const char*>()) {
            return std::nullopt;
        }
        hello.supported_roles.emplace_back(role.as<const char*>());
        const std::string& listed = hello.supported_roles.back();
        if (!is_application_specific(listed) &&
            std::find(KNOWN_ROLES.begin(), KNOWN_ROLES.end(), listed) == KNOWN_ROLES.end()) {
            hello.unrecognized_identifiers.push_back(listed);
        }
    }

    // An invalid optional object counts as absent
    if (!payload["device_info"].isNull()) {
        hello.device_info = parse_device_info(payload["device_info"]).value_or(DeviceInfo{});
    }
    if (is_listed(hello.supported_roles, "player@v1")) {
        hello.player_v1_support = parse_player_support(payload["player@v1_support"]);
    }
    if (is_listed(hello.supported_roles, "source@v1") &&
        payload["source@v1_support"].is<JsonObjectConst>()) {
        hello.source_v1_support = parse_source_support(payload["source@v1_support"]);
    }
    if (is_listed(hello.supported_roles, "visualizer@v1")) {
        hello.visualizer_v1_support = parse_visualizer_support(payload["visualizer@v1_support"]);
    }
    parse_pair_methods(methods.as<JsonObjectConst>(), hello);
    return hello;
}

// messaging.md "client/goodbye"
std::optional<ClientGoodbye> parse_client_goodbye(JsonObjectConst payload, int64_t rx_us) {
    const JsonVariantConst reason = payload["reason"];
    if (!reason.is<const char*>()) {
        return std::nullopt;
    }
    ClientGoodbye goodbye;
    goodbye.rx_us = rx_us;
    const std::string_view text = reason.as<const char*>();
    for (const auto& [name, value] : GOODBYE_REASONS) {
        if (name == text) {
            goodbye.reason = value;
        }
    }
    return goodbye;
}

// messaging.md "client/time"
std::optional<ClientTime> parse_client_time(JsonObjectConst payload, int64_t rx_us) {
    const JsonVariantConst transmitted = payload["client_transmitted"];
    if (!transmitted.is<int64_t>()) {
        return std::nullopt;
    }
    return ClientTime{transmitted.as<int64_t>(), rx_us};
}

// ==============================================================================
// client/state
// ==============================================================================

constexpr std::array<FlagName, 3> COMMAND_NAMES{{
    {"volume", player_command::VOLUME},
    {"mute", player_command::MUTE},
    {"set_output_delay", player_command::SET_OUTPUT_DELAY},
}};

// roles/player/v1.md "client/state player object"
std::optional<PlayerState> parse_player_state(JsonVariantConst object) {
    if (!object.is<JsonObjectConst>()) {
        return std::nullopt;
    }
    PlayerState state;
    const std::optional<uint32_t> output_delay = read_u32(object["output_delay_ms"]);
    const std::optional<uint32_t> lead = read_u32(object["required_lead_time_ms"]);
    const std::optional<uint32_t> min_buffer = read_u32(object["min_buffer_ms"]);
    const std::optional<uint8_t> commands = read_flags(object["supported_commands"], COMMAND_NAMES);
    if (!output_delay || *output_delay > MAX_OUTPUT_DELAY_MS || !lead || !min_buffer || !commands) {
        return std::nullopt;
    }
    state.output_delay_ms = *output_delay;
    state.required_lead_time_ms = *lead;
    state.min_buffer_ms = *min_buffer;
    state.supported_commands = *commands;

    const JsonVariantConst volume = object["volume"];
    if (!volume.isNull()) {
        const std::optional<uint32_t> value = read_u32(volume);
        if (!value || *value > MAX_VOLUME) {
            return std::nullopt;
        }
        state.volume = value;
    }
    const JsonVariantConst muted = object["muted"];
    if (!muted.isNull()) {
        if (!muted.is<bool>()) {
            return std::nullopt;
        }
        state.muted = muted.as<bool>();
    }
    // Required with the matching command
    if (((state.supported_commands & player_command::VOLUME) != 0 && !state.volume) ||
        ((state.supported_commands & player_command::MUTE) != 0 && !state.muted)) {
        return std::nullopt;
    }
    if (!object["format"].isNull()) {
        const std::optional<std::optional<AudioFormat>> format = parse_format(object["format"]);
        if (!format) {
            return std::nullopt;
        }
        state.format = *format;
    }
    return state;
}

// roles/artwork/v1.md "client/state artwork object": 1 to 4 channels, each `none` or a source with
// its format and size. A source or format outside artwork@v1's values fails it, as does a size
// of 0; a channel past the fourth is a protocol error the spec names.
std::optional<std::vector<ArtworkChannel>> parse_artwork_state(JsonVariantConst object) {
    if (!object.is<JsonObjectConst>() || !object["channels"].is<JsonArrayConst>()) {
        return std::nullopt;
    }
    const JsonArrayConst entries = object["channels"].as<JsonArrayConst>();
    if (entries.size() == 0 || entries.size() > MAX_ARTWORK_CHANNELS) {
        return std::nullopt;
    }
    std::vector<ArtworkChannel> channels;
    for (const JsonVariantConst entry : entries) {
        if (!entry.is<JsonObjectConst>() || !entry["source"].is<const char*>()) {
            return std::nullopt;
        }
        const std::string_view source = entry["source"].as<const char*>();
        const JsonVariantConst format = entry["format"];
        const JsonVariantConst width = entry["width"];
        const JsonVariantConst height = entry["height"];
        ArtworkChannel channel;
        if (source == "none") {
            // The optional fields are ignored, but a present one is validated as its siblings are
            if ((!format.isNull() && !format.is<const char*>()) ||
                (!width.isNull() && !read_positive_u32(width)) ||
                (!height.isNull() && !read_positive_u32(height))) {
                return std::nullopt;
            }
            channels.push_back(channel);
            continue;
        }
        if (source == "album") {
            channel.source = ArtworkSource::ALBUM;
        } else if (source == "artist") {
            channel.source = ArtworkSource::ARTIST;
        } else {
            return std::nullopt;
        }
        const std::optional<uint32_t> w = read_positive_u32(width);
        const std::optional<uint32_t> h = read_positive_u32(height);
        if (!format.is<const char*>() || !w || !h) {
            return std::nullopt;
        }
        const std::string_view name = format.as<const char*>();
        if (name == "jpeg") {
            channel.format = ImageFormat::JPEG;
        } else if (name == "png") {
            channel.format = ImageFormat::PNG;
        } else {
            return std::nullopt;
        }
        channel.width = *w;
        channel.height = *h;
        channels.push_back(channel);
    }
    return channels;
}

constexpr std::array<FlagName, 5> VISUALIZER_TYPE_NAMES{{
    {"loudness", visualizer_type::LOUDNESS},
    {"beat", visualizer_type::BEAT},
    {"f_peak", visualizer_type::F_PEAK},
    {"spectrum", visualizer_type::SPECTRUM},
    {"peak", visualizer_type::PEAK},
}};

/// An integer of any size: a field the spec types as an integer, whose value may still lie outside
/// what the server reads
bool is_integer(JsonVariantConst value) {
    return value.is<int64_t>() || value.is<uint64_t>();
}

// roles/visualizer/v1.md "client/state visualizer object": `types` an array of strings (an
// unrecognized type adds none), `rate_max` a positive integer, and `spectrum` an object of three
// integers and a scale string, required when `types` lists 'spectrum' (its absence is the protocol
// error the spec names). A scale the server does not know, or an integer outside uint32, is a
// spectrum it cannot stream, not a malformed one; whether it can stream a valid spectrum is
// negotiate_visualizer()'s to decide.
std::optional<VisualizerRequest> parse_visualizer_state(JsonVariantConst object) {
    if (!object.is<JsonObjectConst>()) {
        return std::nullopt;
    }
    const std::optional<uint8_t> types = read_flags(object["types"], VISUALIZER_TYPE_NAMES);
    const std::optional<uint32_t> rate_max = read_positive_u32(object["rate_max"]);
    if (!types || !rate_max) {
        return std::nullopt;
    }
    VisualizerRequest request{*types, *rate_max, std::nullopt};
    const JsonVariantConst spectrum = object["spectrum"];
    if (!spectrum.isNull()) {
        if (!spectrum.is<JsonObjectConst>() || !spectrum["scale"].is<const char*>() ||
            !is_integer(spectrum["n_disp_bins"]) || !is_integer(spectrum["f_min"]) ||
            !is_integer(spectrum["f_max"])) {
            return std::nullopt;
        }
        const std::optional<uint32_t> bins = read_u32(spectrum["n_disp_bins"]);
        const std::optional<uint32_t> f_min = read_u32(spectrum["f_min"]);
        const std::optional<uint32_t> f_max = read_u32(spectrum["f_max"]);
        const std::string_view scale = spectrum["scale"].as<const char*>();
        SpectrumConfig config;
        bool known = true;
        if (scale == "mel") {
            config.scale = SpectrumScale::MEL;
        } else if (scale == "log") {
            config.scale = SpectrumScale::LOG;
        } else if (scale == "lin") {
            config.scale = SpectrumScale::LIN;
        } else {
            known = false;
        }
        if (known && bins && f_min && f_max) {
            config.n_disp_bins = *bins;
            config.f_min = *f_min;
            config.f_max = *f_max;
            request.spectrum = config;
        } else {
            request.spectrum_unusable = true;
        }
    }
    if ((request.types & visualizer_type::SPECTRUM) != 0 && !request.spectrum &&
        !request.spectrum_unusable) {
        return std::nullopt;
    }
    return request;
}

// roles/source/v1.md "client/state source object": `signal`, when present, a string; one this
// server does not recognize, which a newer revision may add, reads as not reported
std::optional<SourceState> parse_source_state(JsonVariantConst object) {
    if (!object.is<JsonObjectConst>()) {
        return std::nullopt;
    }
    SourceState state;
    const JsonVariantConst signal = object["signal"];
    if (signal.isNull()) {
        return state;
    }
    if (!signal.is<const char*>()) {
        return std::nullopt;
    }
    const std::string_view value = signal.as<const char*>();
    if (value == "present") {
        state.signal = SourceSignal::PRESENT;
    } else if (value == "absent") {
        state.signal = SourceSignal::ABSENT;
    }
    return state;
}

// messaging.md "client/state": `available` and the full state of each role object included
std::optional<ClientState> parse_client_state(JsonObjectConst payload, int64_t rx_us) {
    const JsonVariantConst available = payload["available"];
    if (!available.is<bool>()) {
        return std::nullopt;
    }
    ClientState state;
    state.available = available.as<bool>();
    state.rx_us = rx_us;
    if (!payload["player"].isNull()) {
        state.player = parse_player_state(payload["player"]);
        state.player_malformed = !state.player;
    }
    if (!payload["artwork"].isNull()) {
        state.artwork = parse_artwork_state(payload["artwork"]);
        const JsonVariantConst channels = payload["artwork"]["channels"];
        state.artwork_too_many_channels =
            channels.is<JsonArrayConst>() && channels.size() > MAX_ARTWORK_CHANNELS;
    }
    if (!payload["visualizer"].isNull()) {
        state.visualizer = parse_visualizer_state(payload["visualizer"]);
        state.visualizer_malformed = !state.visualizer;
    }
    if (!payload["source"].isNull()) {
        state.source = parse_source_state(payload["source"]);
        state.source_malformed = !state.source;
    }
    return state;
}

// ==============================================================================
// client-stream/start
// ==============================================================================

/// The bytes of a standard Base64 string with its padding (RFC 4648 section 4)
std::optional<std::vector<uint8_t>> read_base64(JsonVariantConst value) {
    if (!value.is<const char*>()) {
        return std::nullopt;
    }
    std::string text = value.as<std::string>();
    if (text.empty() || text.size() % 4 != 0 || text.find_first_of("-_") != std::string::npos) {
        return std::nullopt;
    }
    std::replace(text.begin(), text.end(), '+', '-');
    std::replace(text.begin(), text.end(), '/', '_');
    return b64url_decode(text);
}

/// RFC 9639 section 8.2: `fLaC`, then a STREAMINFO block (type 0, 34 bytes) of rate, channels and
/// depth as format announces them; later metadata blocks are the decoder's to read
bool is_flac_header_of(std::span<const uint8_t> header, const AudioFormat& format) {
    constexpr size_t MARKER = 4;
    constexpr size_t BLOCK_HEADER = 4;
    constexpr size_t STREAMINFO_BYTES = 34;
    constexpr uint8_t BLOCK_TYPE_MASK = 0x7F;
    if (header.size() < MARKER + BLOCK_HEADER + STREAMINFO_BYTES ||
        std::memcmp(header.data(), "fLaC", MARKER) != 0 ||
        (header[MARKER] & BLOCK_TYPE_MASK) != 0) {
        return false;
    }
    const size_t length =
        (size_t{header[MARKER + 1]} << 16) | (size_t{header[MARKER + 2]} << 8) | header[MARKER + 3];
    // NOLINTBEGIN(readability-magic-numbers): RFC 9639 section 8.2's bit layout
    const std::span<const uint8_t> info = header.subspan(MARKER + BLOCK_HEADER);
    const uint32_t rate =
        (uint32_t{info[10]} << 12) | (uint32_t{info[11]} << 4) | (uint32_t{info[12]} >> 4);
    const uint32_t channels = ((info[12] >> 1) & 0x07U) + 1;
    const uint32_t depth = (((info[12] & 0x01U) << 4) | (uint32_t{info[13]} >> 4)) + 1;
    // NOLINTEND(readability-magic-numbers)
    return length == STREAMINFO_BYTES && rate == format.sample_rate &&
           channels == format.channels && depth == format.bit_depth;
}

// roles/source/v1.md "client-stream/start": a source announces only a codec the server lists, so
// an unrecognized one is malformed rather than skipped
std::optional<ClientStreamStart> parse_client_stream_start(JsonObjectConst payload, int64_t rx_us) {
    const JsonVariantConst source = payload["source"];
    const JsonVariantConst codec = source["codec"];
    if (!source.is<JsonObjectConst>() || !codec.is<const char*>()) {
        return std::nullopt;
    }
    const std::optional<Codec> known = codec_from_wire(codec.as<const char*>());
    const std::optional<uint32_t> channels = read_positive_u32(source["channels"]);
    const std::optional<uint32_t> sample_rate = read_positive_u32(source["sample_rate"]);
    if (!known || !channels || !sample_rate) {
        return std::nullopt;
    }
    ClientStreamStart start;
    start.format = AudioFormat{*known, *channels, *sample_rate, 0};
    start.rx_us = rx_us;
    if (*known != Codec::OPUS) {
        const std::optional<uint32_t> bit_depth = read_positive_u32(source["bit_depth"]);
        if (!bit_depth) {
            return std::nullopt;
        }
        start.format.bit_depth = *bit_depth;
    }
    const JsonVariantConst header = source["codec_header"];
    if (*known != Codec::FLAC) {
        return header.isNull() ? std::optional(std::move(start)) : std::nullopt;
    }
    std::optional<std::vector<uint8_t>> bytes = read_base64(header);
    if (!bytes || !is_flac_header_of(*bytes, start.format)) {
        return std::nullopt;
    }
    start.codec_header = std::move(*bytes);
    return start;
}

// ==============================================================================
// client/command
// ==============================================================================

constexpr std::array<std::string_view, CONTROLLER_COMMAND_COUNT> CONTROLLER_COMMAND_NAMES{
    "play",    "pause",     "stop",       "next",       "previous",
    "volume",  "mute",      "repeat_off", "repeat_one", "repeat_all",
    "shuffle", "unshuffle", "switch",     "seek",       "seek_relative",
};

std::optional<int64_t> read_i64(JsonVariantConst value) {
    if (!value.is<int64_t>()) {
        return std::nullopt;
    }
    return value.as<int64_t>();
}

// roles/controller/v1.md "client/command controller object": a recognized command carries its
// parameter, or is marked malformed; an unrecognized one is kept as such, since no
// supported_commands lists it
std::optional<ControllerRequest> parse_controller_request(JsonVariantConst object) {
    const JsonVariantConst command = object["command"];
    if (!object.is<JsonObjectConst>() || !command.is<const char*>()) {
        return std::nullopt;
    }
    ControllerRequest request;
    const std::string_view name = command.as<const char*>();
    for (size_t i = 0; i < CONTROLLER_COMMAND_NAMES.size(); ++i) {
        if (CONTROLLER_COMMAND_NAMES[i] == name) {
            request.command = static_cast<ControllerCommand>(i);
        }
    }
    if (!request.command) {
        return request;
    }
    switch (*request.command) {
        case ControllerCommand::VOLUME:
            request.volume = read_u32(object["volume"]);
            if (request.volume && *request.volume > MAX_VOLUME) {
                request.volume.reset();
            }
            request.malformed = !request.volume;
            break;
        case ControllerCommand::MUTE:
            if (object["mute"].is<bool>()) {
                request.mute = object["mute"].as<bool>();
            }
            request.malformed = !request.mute;
            break;
        case ControllerCommand::SEEK:
            request.position_ms = read_i64(object["position_ms"]);
            request.malformed = !request.position_ms;
            break;
        case ControllerCommand::SEEK_RELATIVE:
            request.offset_ms = read_i64(object["offset_ms"]);
            request.malformed = !request.offset_ms;
            break;
        default:
            break;
    }
    return request;
}

// messaging.md "client/command"
ClientCommand parse_client_command(JsonObjectConst payload, int64_t rx_us) {
    ClientCommand command;
    command.rx_us = rx_us;
    if (!payload["controller"].isNull()) {
        command.controller = parse_controller_request(payload["controller"]);
        command.controller_malformed = !command.controller;
    }
    return command;
}

// ==============================================================================
// server/state
// ==============================================================================

std::string_view to_wire(RepeatMode mode) {
    switch (mode) {
        case RepeatMode::ONE:
            return "one";
        case RepeatMode::ALL:
            return "all";
        case RepeatMode::OFF:
            break;
    }
    return "off";
}

void add_optional(JsonObject object, const char* key, const std::optional<std::string>& value) {
    if (value) {
        object[key] = *value;
    }
}

void add_optional(JsonObject object, const char* key, const std::optional<uint32_t>& value) {
    if (value) {
        object[key] = *value;
    }
}

/// roles/color/v1.md: a color is `[R, G, B]`
void add_optional(JsonObject object, const char* key, const std::optional<RgbColor>& value) {
    if (value) {
        JsonArray rgb = object[key].to<JsonArray>();
        rgb.add(value->r);
        rgb.add(value->g);
        rgb.add(value->b);
    }
}

// ==============================================================================
// Pairing
// ==============================================================================

constexpr std::array<std::pair<std::string_view, PairAbortReason>, 5> ABORT_REASONS{{
    {"attempt_timeout", PairAbortReason::ATTEMPT_TIMEOUT},
    {"concurrent_attempt", PairAbortReason::CONCURRENT_ATTEMPT},
    {"method_not_supported", PairAbortReason::METHOD_NOT_SUPPORTED},
    {"pairing_code_mismatch", PairAbortReason::PAIRING_CODE_MISMATCH},
    {"user_cancelled", PairAbortReason::USER_CANCELLED},
}};

/// The pairing fields the message trace never shows (pairing.md "Wrapping": sealed values;
/// "client/pair-finalize": the long-term PSK)
constexpr std::array<std::string_view, 3> SECRET_FIELDS{"long_term_psk", "wrapped_psk",
                                                        "wrapped_nonce_B"};
constexpr std::string_view REDACTED = "<redacted>";

/// The bytes of an unpadded base64url field of exactly N bytes (pairing.md "PAKE": each field's
/// length is fixed); nullopt for anything else
template <size_t N>
std::optional<std::array<uint8_t, N>> read_b64_fixed(JsonVariantConst value) {
    constexpr size_t CHARS = (N * 8 + 5) / 6;
    if (!value.is<const char*>()) {
        return std::nullopt;
    }
    const std::string_view text = value.as<const char*>();
    if (text.size() != CHARS) {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> bytes = b64url_decode(text);
    if (!bytes || bytes->size() != N) {
        if (bytes) {
            secure_zero(bytes->data(), bytes->size());
        }
        return std::nullopt;
    }
    std::array<uint8_t, N> out{};
    std::copy(bytes->begin(), bytes->end(), out.begin());
    secure_zero(bytes->data(), bytes->size());
    return out;
}

/// text cut to at most max_chars UTF-8 characters
std::string cut_characters(std::string_view text, size_t max_chars) {
    constexpr uint8_t CONTINUATION_MASK = 0xC0;
    constexpr uint8_t CONTINUATION = 0x80;
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<uint8_t>(text[i]) & CONTINUATION_MASK) != CONTINUATION) {
            if (chars == max_chars) {
                return std::string(text.substr(0, i));
            }
            ++chars;
        }
    }
    return std::string(text);
}

// pairing.md "Messages": the fields each pairing message carries; a missing or malformed one is a
// protocol error ("Protocol Errors")
std::optional<ClientPairing> parse_client_pairing(PairingMessageKind kind, JsonObjectConst payload,
                                                  int64_t rx_us) {
    ClientPairing message;
    message.kind = kind;
    message.rx_us = rx_us;
    switch (kind) {
        case PairingMessageKind::PENDING:
        case PairingMessageKind::INIT: {
            const std::optional<uint32_t> index = read_u32(payload["pairing_index"]);
            if (!index) {
                return std::nullopt;
            }
            message.pairing_index = *index;
            if (kind == PairingMessageKind::PENDING) {
                std::optional<std::string> text;
                if (!read_optional_string(payload["message"], text)) {
                    return std::nullopt;
                }
                if (text) {
                    message.message = cut_characters(*text, MAX_PAIR_PENDING_CHARS);
                }
            } else if (!payload["commit_B"].isNull()) {
                message.commit_b = read_b64_fixed<PAIRING_COMMIT_SIZE>(payload["commit_B"]);
                if (!message.commit_b) {
                    return std::nullopt;
                }
            }
            return message;
        }
        case PairingMessageKind::AUTH: {
            const auto share = read_b64_fixed<CPACE_SHARE_SIZE>(payload["pake_msg_2"]);
            if (!share) {
                return std::nullopt;
            }
            message.pake_msg_2 = *share;
            return message;
        }
        case PairingMessageKind::RETRY:
            return message;
        case PairingMessageKind::CONFIRM: {
            const auto tag = read_b64_fixed<CPACE_TAG_SIZE>(payload["client_kc"]);
            if (!tag) {
                return std::nullopt;
            }
            message.client_kc = *tag;
            if (!payload["wrapped_nonce_B"].isNull()) {
                message.wrapped_nonce_b =
                    read_b64_fixed<WRAPPED_VALUE_SIZE>(payload["wrapped_nonce_B"]);
                if (!message.wrapped_nonce_b) {
                    return std::nullopt;
                }
            }
            return message;
        }
        case PairingMessageKind::FINALIZE: {
            const JsonVariantConst plain = payload["long_term_psk"];
            const JsonVariantConst wrapped = payload["wrapped_psk"];
            // pairing.md "client/pair-finalize": exactly one of the two
            if (plain.isNull() == wrapped.isNull()) {
                return std::nullopt;
            }
            if (!plain.isNull()) {
                std::optional<Psk> psk = read_b64_fixed<PSK_SIZE>(plain);
                if (!psk) {
                    return std::nullopt;
                }
                message.long_term_psk.emplace(PskCategory::LONG_TERM, *psk);
                secure_zero(psk->data(), psk->size());
                return message;
            }
            message.wrapped_psk = read_b64_fixed<WRAPPED_VALUE_SIZE>(wrapped);
            if (!message.wrapped_psk) {
                return std::nullopt;
            }
            return message;
        }
        case PairingMessageKind::ABORT: {
            const JsonVariantConst reason = payload["reason"];
            if (!reason.is<const char*>()) {
                return std::nullopt;
            }
            const std::string_view text = reason.as<const char*>();
            for (const auto& [name, value] : ABORT_REASONS) {
                if (name == text) {
                    message.abort_reason = value;
                }
            }
            return message;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::pair<std::string_view, PairingMessageKind>, 7> PAIRING_TYPES{{
    {"client/pair-pending", PairingMessageKind::PENDING},
    {"client/pair-init", PairingMessageKind::INIT},
    {"client/pair-auth", PairingMessageKind::AUTH},
    {"client/pair-retry", PairingMessageKind::RETRY},
    {"client/pair-confirm", PairingMessageKind::CONFIRM},
    {"client/pair-finalize", PairingMessageKind::FINALIZE},
    {"pair/abort", PairingMessageKind::ABORT},
}};

std::string_view to_wire(PairMethod method) {
    switch (method) {
        case PairMethod::PAIRING_PSK:
            return "pairing_psk";
        case PairMethod::STATIC_PAIRING_CODE:
            return "static_pairing_code";
        case PairMethod::DYNAMIC_PAIRING_CODE:
            break;
    }
    return "dynamic_pairing_code";
}

std::string b64(std::span<const uint8_t> bytes) {
    return b64url_encode(bytes.data(), bytes.size());
}

// Recognized types a client may never send once activated: the server's own messages
// (messaging.md "Communication": `server/` names the sender; `stream/` and `group/` messages are
// defined server to client), the cleartext handshake, a second client/hello (sent once per
// connection), and noise/handshake, which only answers a re-handshake the server started
// (connection.md "Re-handshake")
constexpr std::array<std::string_view, 19> CLOSING_TYPES{
    "client/init",         "client/hello",         "noise/handshake",  "server/init",
    "server/error",        "server/hello",         "server/activate",  "server/time",
    "server/state",        "server/command",       "server/unpair",    "server/pair-auth",
    "server/pair-confirm", "server/pair-finalize", "server/pair-init", "stream/start",
    "stream/clear",        "stream/end",           "group/update",
};

}  // namespace

std::string_view to_wire(GoodbyeReason reason) {
    for (const auto& [name, value] : GOODBYE_REASONS) {
        if (value == reason) {
            return name;
        }
    }
    return {};
}

// ==============================================================================
// Cleartext handshake
// ==============================================================================

std::string_view to_wire(Codec codec) {
    switch (codec) {
        case Codec::OPUS:
            return "opus";
        case Codec::FLAC:
            return "flac";
        case Codec::PCM:
            break;
    }
    return "pcm";
}

std::string_view to_wire(ControllerCommand command) {
    return CONTROLLER_COMMAND_NAMES[static_cast<size_t>(command)];
}

std::string_view to_wire(InitError error) {
    switch (error) {
        case InitError::UNSUPPORTED_VERSION:
            return "unsupported_version";
        case InitError::UNSUPPORTED_SUITE:
            return "unsupported_suite";
        case InitError::MALFORMED:
            break;
    }
    return "malformed";
}

// connection.md "Failure Handling"
std::variant<ClientInit, InitError> validate_client_init(std::string_view text, int64_t rx_us) {
    // ArduinoJson is lenient (single quotes, unquoted keys, trailing bytes); harmless, since the
    // prologue binds the raw bytes and the client hashed exactly those
    JsonDocument doc;
    if (deserializeJson(doc, text.data(), text.size()) != DeserializationError::Ok) {
        return InitError::MALFORMED;
    }
    const JsonVariantConst type = doc["type"];
    const JsonVariantConst payload = doc["payload"];
    if (!doc.is<JsonObjectConst>() || !type.is<const char*>() ||
        std::string_view(type.as<const char*>()) != "client/init" ||
        !payload.is<JsonObjectConst>()) {
        return InitError::MALFORMED;
    }

    const JsonVariantConst version = payload["version"];
    if (version.is<int64_t>()) {
        if (version.as<int64_t>() != PROTOCOL_VERSION) {
            return InitError::UNSUPPORTED_VERSION;
        }
    } else if (version.is<uint64_t>()) {
        return InitError::UNSUPPORTED_VERSION;
    } else {
        return InitError::MALFORMED;
    }

    const JsonVariantConst suite_name = payload["suite"];
    if (!suite_name.is<const char*>()) {
        return InitError::MALFORMED;
    }
    const std::optional<CipherSuite> suite = cipher_suite_from_wire(suite_name.as<const char*>());
    if (!suite) {
        return InitError::UNSUPPORTED_SUITE;
    }

    const JsonVariantConst client_id = payload["client_id"];
    if (!client_id.is<const char*>()) {
        return InitError::MALFORMED;
    }
    const std::optional<PublicKey> key = public_key_from_peer_id(client_id.as<const char*>());
    if (!key) {
        return InitError::MALFORMED;
    }
    return ClientInit{client_id.as<std::string>(), *key, *suite, rx_us};
}

std::string build_server_init(std::string_view server_id) {
    JsonDocument doc;
    doc["type"] = "server/init";
    doc["payload"]["server_id"] = server_id;
    doc["payload"]["version"] = PROTOCOL_VERSION;
    return serialize(doc);
}

std::string build_server_error(InitError error) {
    JsonDocument doc;
    doc["type"] = "server/error";
    doc["payload"]["reason"] = to_wire(error);
    return serialize(doc);
}

std::string build_noise_handshake(std::span<const uint8_t> noise_message) {
    JsonDocument doc;
    doc["type"] = "noise/handshake";
    doc["payload"]["data"] = b64url_encode(noise_message.data(), noise_message.size());
    return serialize(doc);
}

std::optional<std::vector<uint8_t>> parse_noise_handshake(std::string_view text) {
    JsonDocument doc;
    if (deserializeJson(doc, text.data(), text.size()) != DeserializationError::Ok) {
        return std::nullopt;
    }
    const JsonVariantConst type = doc["type"];
    const JsonVariantConst data = doc["payload"]["data"];
    if (!type.is<const char*>() || std::string_view(type.as<const char*>()) != "noise/handshake" ||
        !data.is<const char*>()) {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> bytes = b64url_decode(data.as<const char*>());
    if (!bytes || bytes->empty()) {
        return std::nullopt;
    }
    return bytes;
}

std::string build_message_1_payload(std::string_view psk_id, PskCategory category) {
    JsonDocument doc;
    doc["psk_id"] = psk_id;
    doc["psk_category"] = to_wire(category);
    return serialize(doc);
}

bool is_message_2_payload(std::span<const uint8_t> payload) {
    return payload.size() == 2 && payload[0] == '{' && payload[1] == '}';
}

// ==============================================================================
// server/hello
// ==============================================================================

// messaging.md "server/hello": `languages`, when present, is non-empty
std::optional<std::string> build_server_hello(const ServerHello& hello) {
    JsonDocument doc;
    doc["type"] = "server/hello";
    doc["payload"]["name"] = hello.name;
    if (!hello.languages.empty()) {
        JsonArray languages = doc["payload"]["languages"].to<JsonArray>();
        for (const std::string& language : hello.languages) {
            languages.add(language);
        }
    }
    if (!hello.source_codecs.empty()) {
        // roles/source/v1.md "server/hello source@v1 support object"
        JsonArray codecs = doc["payload"]["source@v1_support"]["supported_codecs"].to<JsonArray>();
        for (const Codec codec : hello.source_codecs) {
            codecs.add(to_wire(codec));
        }
    }
    return try_serialize(doc);
}

// ==============================================================================
// server/activate
// ==============================================================================

// messaging.md "server/activate"
std::optional<std::string> build_server_activate(const ServerActivate& activate) {
    JsonDocument doc;
    doc["type"] = "server/activate";
    JsonArray activities = doc["payload"]["activities"].to<JsonArray>();
    if (activate.playback) {
        activities.add("playback");
    }
    if (activate.pairing) {
        activities.add("pairing");
    }
    if (activate.active_roles) {
        JsonArray roles = doc["payload"]["active_roles"].to<JsonArray>();
        for (const std::string& role : *activate.active_roles) {
            roles.add(role);
        }
    }
    if (activate.pairing) {
        JsonObject pairing = doc["payload"]["pairing"].to<JsonObject>();
        pairing["method"] = to_wire(activate.pairing->method);
        if (activate.pairing->format) {
            pairing["format"] =
                *activate.pairing->format == CodeFormat::QR_CODE ? "qr_code" : "digits";
        }
    }
    return try_serialize(doc);
}

// ==============================================================================
// Inbound application messages
// ==============================================================================

ClientMessage parse_client_message(std::string_view json, int64_t rx_us) {
    // A pairing message may carry a secret
    JsonDocument doc(WipingAllocator::instance());
    if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok) {
        return MalformedMessage{};
    }
    const JsonVariantConst type = doc["type"];
    if (!doc.is<JsonObjectConst>() || !type.is<const char*>()) {
        return MalformedMessage{};
    }
    std::string type_name = type.as<std::string>();
    const JsonVariantConst payload = doc["payload"];
    if (!payload.is<JsonObjectConst>()) {
        return MalformedMessage{std::move(type_name)};
    }
    if (type_name == "client/hello") {
        if (std::optional<ClientHello> hello =
                parse_client_hello(payload.as<JsonObjectConst>(), rx_us)) {
            return std::move(*hello);
        }
        return MalformedMessage{std::move(type_name)};
    }
    if (type_name == "client/goodbye") {
        if (std::optional<ClientGoodbye> goodbye =
                parse_client_goodbye(payload.as<JsonObjectConst>(), rx_us)) {
            return *goodbye;
        }
        return MalformedMessage{std::move(type_name)};
    }
    if (type_name == "client/time") {
        if (std::optional<ClientTime> time =
                parse_client_time(payload.as<JsonObjectConst>(), rx_us)) {
            return *time;
        }
        return MalformedMessage{std::move(type_name)};
    }
    if (type_name == "client/command") {
        return parse_client_command(payload.as<JsonObjectConst>(), rx_us);
    }
    if (type_name == "client/leave") {
        return ClientLeave{rx_us};
    }
    for (const auto& [name, kind] : PAIRING_TYPES) {
        if (type_name == name) {
            if (std::optional<ClientPairing> pairing =
                    parse_client_pairing(kind, payload.as<JsonObjectConst>(), rx_us)) {
                return std::move(*pairing);
            }
            return MalformedMessage{std::move(type_name)};
        }
    }
    if (type_name == "client-stream/start") {
        if (std::optional<ClientStreamStart> start =
                parse_client_stream_start(payload.as<JsonObjectConst>(), rx_us)) {
            return std::move(*start);
        }
        return MalformedMessage{std::move(type_name)};
    }
    if (type_name == "client-stream/end") {
        return ClientStreamEnd{rx_us};
    }
    if (type_name == "client/state") {
        if (std::optional<ClientState> state =
                parse_client_state(payload.as<JsonObjectConst>(), rx_us)) {
            return *state;
        }
        return MalformedMessage{std::move(type_name)};
    }
    return OtherMessage{std::move(type_name), rx_us};
}

UnhandledRule rule_for_unhandled(std::string_view type) {
    return std::find(CLOSING_TYPES.begin(), CLOSING_TYPES.end(), type) == CLOSING_TYPES.end()
               ? UnhandledRule::IGNORE
               : UnhandledRule::CLOSE;
}

// ==============================================================================
// server/time
// ==============================================================================

std::string build_server_time(const ClientTime& request, int64_t transmitted_us) {
    JsonDocument doc;
    doc["type"] = "server/time";
    doc["payload"]["client_transmitted"] = request.client_transmitted;
    doc["payload"]["server_received"] = request.rx_us;
    doc["payload"]["server_transmitted"] = transmitted_us;
    return serialize(doc);
}

// ==============================================================================
// group/update and stream lifecycle
// ==============================================================================

// messaging.md "group/update"
std::optional<std::string> build_group_update(const GroupUpdate& update) {
    JsonDocument doc;
    doc["type"] = "group/update";
    doc["payload"]["playback_state"] = update.playing ? "playing" : "stopped";
    doc["payload"]["group_id"] = update.group_id;
    doc["payload"]["group_name"] = update.group_name;
    return try_serialize(doc);
}

// ==============================================================================
// server/state and server/command
// ==============================================================================

// messaging.md "server/state": the full state of each object included
std::optional<std::string> build_server_state(const ServerState& state) {
    JsonDocument doc;
    doc["type"] = "server/state";
    JsonObject payload = doc["payload"].to<JsonObject>();
    if (state.controller) {
        // roles/controller/v1.md "server/state controller object"
        const ControllerState& controller = *state.controller;
        JsonObject object = payload["controller"].to<JsonObject>();
        JsonArray commands = object["supported_commands"].to<JsonArray>();
        for (size_t i = 0; i < CONTROLLER_COMMAND_COUNT; ++i) {
            if ((controller.supported_commands & command_bit(static_cast<ControllerCommand>(i))) !=
                0) {
                commands.add(CONTROLLER_COMMAND_NAMES[i]);
            }
        }
        object["volume"] = controller.volume;
        object["muted"] = controller.muted;
        object["repeat"] = to_wire(controller.repeat);
        object["shuffle"] = controller.shuffle;
        add_optional(object, "seek_max_ms", controller.seek_max_ms);
    }
    if (state.metadata) {
        // roles/metadata/v1.md "server/state metadata object"
        const Metadata& metadata = state.metadata->metadata;
        JsonObject object = payload["metadata"].to<JsonObject>();
        object["timestamp"] = state.metadata->timestamp_us;
        add_optional(object, "title", metadata.title);
        add_optional(object, "artist", metadata.artist);
        add_optional(object, "album_artist", metadata.album_artist);
        add_optional(object, "album", metadata.album);
        add_optional(object, "artwork_url", metadata.artwork_url);
        add_optional(object, "year", metadata.year);
        add_optional(object, "track", metadata.track);
        if (metadata.progress) {
            JsonObject progress = object["progress"].to<JsonObject>();
            progress["track_progress"] = metadata.progress->track_progress_ms;
            progress["track_duration"] = metadata.progress->track_duration_ms;
            progress["playback_speed"] = metadata.progress->playback_speed;
        }
    }
    if (state.color) {
        // roles/color/v1.md "server/state color object"
        const Colors& colors = state.color->colors;
        JsonObject object = payload["color"].to<JsonObject>();
        object["timestamp"] = state.color->timestamp_us;
        add_optional(object, "background_dark", colors.background_dark);
        add_optional(object, "background_light", colors.background_light);
        add_optional(object, "primary", colors.primary);
        add_optional(object, "accent", colors.accent);
        add_optional(object, "on_dark", colors.on_dark);
        add_optional(object, "on_light", colors.on_light);
    }
    return try_serialize(doc);
}

// roles/player/v1.md "server/command player object"
std::string build_player_volume_command(uint32_t volume) {
    JsonDocument doc;
    doc["type"] = "server/command";
    doc["payload"]["player"]["command"] = "volume";
    doc["payload"]["player"]["volume"] = volume;
    return serialize(doc);
}

std::string build_player_mute_command(bool mute) {
    JsonDocument doc;
    doc["type"] = "server/command";
    doc["payload"]["player"]["command"] = "mute";
    doc["payload"]["player"]["mute"] = mute;
    return serialize(doc);
}

// roles/source/v1.md "server/command source object"
std::string build_source_command(SourceCommand command) {
    JsonDocument doc;
    doc["type"] = "server/command";
    doc["payload"]["source"]["command"] = command == SourceCommand::START ? "start" : "stop";
    return serialize(doc);
}

// roles/source/v1.md "Source Audio Chunks (Binary)": the ID, a big-endian int64 timestamp, then
// the encoded audio
std::optional<SourceChunk> parse_source_chunk(std::span<const uint8_t> message) {
    if (message.size() < SOURCE_CHUNK_HEADER_BYTES) {
        return std::nullopt;
    }
    uint64_t timestamp = 0;
    for (size_t i = 1; i < SOURCE_CHUNK_HEADER_BYTES; ++i) {
        timestamp = (timestamp << 8) | message[i];
    }
    const auto capture_us = static_cast<int64_t>(timestamp);
    if (capture_us > MAX_SOURCE_TIMESTAMP_US || capture_us < -MAX_SOURCE_TIMESTAMP_US) {
        return std::nullopt;
    }
    return SourceChunk{capture_us, message.subspan(SOURCE_CHUNK_HEADER_BYTES)};
}

// messaging.md "stream/start", roles/player/v1.md "stream/start player object",
// roles/visualizer/v1.md "stream/start visualizer object"
std::string build_stream_start(int64_t transmitted_us, const StreamStart& start) {
    JsonDocument doc;
    doc["type"] = "stream/start";
    doc["payload"]["server_transmitted"] = transmitted_us;
    if (start.player) {
        JsonObject player = doc["payload"]["player"].to<JsonObject>();
        player["codec"] = to_wire(start.player->codec);
        player["sample_rate"] = start.player->sample_rate;
        player["channels"] = start.player->channels;
        player["bit_depth"] = start.player->bit_depth;
        if (start.codec_header) {
            player["codec_header"] = *start.codec_header;
        }
    }
    if (start.visualizer) {
        const VisualizerConfig& config = *start.visualizer;
        JsonObject visualizer = doc["payload"]["visualizer"].to<JsonObject>();
        JsonArray types = visualizer["types"].to<JsonArray>();
        for (const FlagName& type : VISUALIZER_TYPE_NAMES) {
            if ((config.types & type.bit) != 0) {
                types.add(type.name);
            }
        }
        visualizer["rate_max"] = config.rate;
        if (config.spectrum) {
            JsonObject spectrum = visualizer["spectrum"].to<JsonObject>();
            spectrum["n_disp_bins"] = config.spectrum->n_disp_bins;
            spectrum["scale"] = config.spectrum->scale == SpectrumScale::MEL   ? "mel"
                                : config.spectrum->scale == SpectrumScale::LOG ? "log"
                                                                               : "lin";
            spectrum["f_min"] = config.spectrum->f_min;
            spectrum["f_max"] = config.spectrum->f_max;
        }
    }
    return serialize(doc);
}

// messaging.md "stream/start", roles/artwork/v1.md "stream/start artwork object"
std::string build_stream_start_artwork(int64_t transmitted_us,
                                       std::span<const ArtworkChannel> channels) {
    JsonDocument doc;
    doc["type"] = "stream/start";
    doc["payload"]["server_transmitted"] = transmitted_us;
    JsonArray listed = doc["payload"]["artwork"]["channels"].to<JsonArray>();
    for (const ArtworkChannel& channel : channels) {
        JsonObject entry = listed.add<JsonObject>();
        if (!channel.source) {
            entry["source"] = "none";
            continue;
        }
        entry["source"] = *channel.source == ArtworkSource::ALBUM ? "album" : "artist";
        entry["format"] = channel.format == ImageFormat::JPEG ? "jpeg" : "png";
        entry["width"] = channel.width;
        entry["height"] = channel.height;
    }
    return serialize(doc);
}

// messaging.md "stream/clear"
std::string build_stream_clear(int64_t transmitted_us, std::span<const std::string_view> roles) {
    JsonDocument doc;
    doc["type"] = "stream/clear";
    doc["payload"]["server_transmitted"] = transmitted_us;
    JsonArray listed = doc["payload"]["roles"].to<JsonArray>();
    for (const std::string_view role : roles) {
        listed.add(role);
    }
    return serialize(doc);
}

// messaging.md "stream/end"
std::string build_stream_end(std::span<const std::string_view> roles) {
    JsonDocument doc;
    doc["type"] = "stream/end";
    JsonArray listed = doc["payload"]["roles"].to<JsonArray>();
    for (const std::string_view role : roles) {
        listed.add(role);
    }
    return serialize(doc);
}

// ==============================================================================
// Pairing messages the server sends
// ==============================================================================

// pairing.md "server/pair-init"
std::string build_server_pair_init(
    const std::optional<std::array<uint8_t, PAIRING_NONCE_SIZE>>& nonce_a) {
    JsonDocument doc;
    doc["type"] = "server/pair-init";
    JsonObject payload = doc["payload"].to<JsonObject>();
    if (nonce_a) {
        payload["nonce_A"] = b64(*nonce_a);
    }
    return serialize(doc);
}

std::string build_server_pair_auth(const std::array<uint8_t, CPACE_SHARE_SIZE>& pake_msg_1) {
    JsonDocument doc;
    doc["type"] = "server/pair-auth";
    doc["payload"]["pake_msg_1"] = b64(pake_msg_1);
    return serialize(doc);
}

std::string build_server_pair_confirm(const std::array<uint8_t, CPACE_TAG_SIZE>& server_kc) {
    JsonDocument doc;
    doc["type"] = "server/pair-confirm";
    doc["payload"]["server_kc"] = b64(server_kc);
    return serialize(doc);
}

std::string build_server_pair_finalize() {
    JsonDocument doc;
    doc["type"] = "server/pair-finalize";
    doc["payload"].to<JsonObject>();
    return serialize(doc);
}

std::string build_pair_abort(PairAbortReason reason) {
    JsonDocument doc;
    doc["type"] = "pair/abort";
    for (const auto& [name, value] : ABORT_REASONS) {
        if (value == reason) {
            doc["payload"]["reason"] = name;
        }
    }
    return serialize(doc);
}

// messaging.md "server/unpair": no payload fields
std::string build_server_unpair() {
    JsonDocument doc;
    doc["type"] = "server/unpair";
    doc["payload"].to<JsonObject>();
    return serialize(doc);
}

std::optional<std::vector<uint8_t>> redact_pairing_secrets(std::span<const uint8_t> plaintext) {
    if (plaintext.empty() || plaintext.front() != MESSAGE_ID_JSON) {
        return std::nullopt;
    }
    const std::string_view json(reinterpret_cast<const char*>(plaintext.data() + 1),
                                plaintext.size() - 1);
    // A key may be spelled with JSON escapes, so a body with a backslash is parsed too
    const bool named = std::any_of(
        SECRET_FIELDS.begin(), SECRET_FIELDS.end(),
        [json](std::string_view field) { return json.find(field) != std::string_view::npos; });
    if (!named && json.find('\\') == std::string_view::npos) {
        return std::nullopt;
    }
    std::vector<uint8_t> redacted{MESSAGE_ID_JSON};
    JsonDocument doc(WipingAllocator::instance());
    if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok ||
        !doc["payload"].is<JsonObject>()) {
        redacted.insert(redacted.end(), REDACTED.begin(), REDACTED.end());
        return redacted;
    }
    JsonObject payload = doc["payload"].as<JsonObject>();
    bool found = false;
    for (const std::string_view field : SECRET_FIELDS) {
        if (!payload[field].isNull()) {
            payload[field] = REDACTED;
            found = true;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    std::string out;
    serializeJson(doc, out);
    redacted.insert(redacted.end(), out.begin(), out.end());
    return redacted;
}

}  // namespace sendspin::server
