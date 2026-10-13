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

/// @file web_commands.cpp
/// @brief The dashboard's commands, state, media directory and request checks

#include "web_commands.h"

#include "one_group.h"
#include <ArduinoJson.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace sendspin::server::examples {

namespace {

/// The longest client_id, URL, code or token a command may carry
constexpr size_t MAX_FIELD_BYTES = 2048;
/// The longest media path a command may carry
constexpr size_t MAX_PATH_BYTES = 4096;
constexpr uint32_t MAX_VOLUME = 100;

// ==============================================================================
// Request checks
// ==============================================================================

/// host without its port: "[::1]:8080" gives "::1", "localhost:8080" gives "localhost"
std::string_view host_name(std::string_view host) {
    if (host.starts_with('[')) {
        const size_t close = host.find(']');
        return close == std::string_view::npos ? std::string_view() : host.substr(1, close - 1);
    }
    return host.substr(0, host.find(':'));
}

std::string lowercase(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

bool loopback_name(std::string_view name) {
    const std::string lower = lowercase(name);
    return lower == "localhost" || lower == "127.0.0.1" || lower == "::1";
}

/// A name no other site's page can be reached under: an IP address, localhost, or a multicast DNS
/// name (RFC 6762 section 3), which the network's own devices answer for
bool local_name(std::string_view name) {
    const std::string lower = lowercase(name);
    std::array<unsigned char, sizeof(in6_addr)> address{};
    constexpr std::string_view MDNS_SUFFIX = ".local";
    return ::inet_pton(AF_INET, lower.c_str(), address.data()) == 1 ||
           ::inet_pton(AF_INET6, lower.c_str(), address.data()) == 1 || lower == "localhost" ||
           (lower.size() > MDNS_SUFFIX.size() && lower.ends_with(MDNS_SUFFIX));
}

/// The value of name in an HTTP target's query, as given; empty when absent
std::string_view query_value(std::string_view target, std::string_view name) {
    const size_t question = target.find('?');
    std::string_view query = question == std::string_view::npos ? "" : target.substr(question + 1);
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        if (pair.size() > name.size() && pair.starts_with(name) && pair[name.size()] == '=') {
            return pair.substr(name.size() + 1);
        }
        query = amp == std::string_view::npos ? "" : query.substr(amp + 1);
    }
    return {};
}

// ==============================================================================
// Command fields
// ==============================================================================

/// A string field of at most max_bytes, not empty
std::optional<std::string> text_field(JsonObjectConst args, const char* key, size_t max_bytes) {
    const JsonVariantConst value = args[key];
    if (!value.is<const char*>()) {
        return std::nullopt;
    }
    std::string text = value.as<std::string>();
    if (text.empty() || text.size() > max_bytes) {
        return std::nullopt;
    }
    return text;
}

std::optional<bool> bool_field(JsonObjectConst args, const char* key) {
    const JsonVariantConst value = args[key];
    if (!value.is<bool>()) {
        return std::nullopt;
    }
    return value.as<bool>();
}

/// The parameters of every command, each read once its command needs it
struct Context {
    SendspinServer& server;
    WebPlayback& playback;
    const MediaDir* media;
    std::atomic<bool>& one_group;
    bool one_group_fixed;
};

/// A command's outcome: an error, or success with data added to the result
using Outcome = std::optional<std::string>;

/// The group a command names, when groups() lists it
std::optional<GroupId> group_field(const Context& context, JsonObjectConst args) {
    const JsonVariantConst value = args["group"];
    if (!value.is<uint64_t>()) {
        return std::nullopt;
    }
    const GroupId id{value.as<uint64_t>()};
    const std::vector<GroupInfo> groups = context.server.groups();
    if (std::none_of(groups.begin(), groups.end(),
                     [&](const GroupInfo& group) { return group.id == id; })) {
        return std::nullopt;
    }
    return id;
}

/// A stream format object: codec, sample_rate, channels and bit_depth
std::optional<StreamFormat> format_field(JsonObjectConst format) {
    constexpr std::array<std::pair<std::string_view, StreamCodec>, 3> CODECS{{
        {"pcm", StreamCodec::PCM},
        {"flac", StreamCodec::FLAC},
        {"opus", StreamCodec::OPUS},
    }};
    const JsonVariantConst codec = format["codec"];
    const auto found = std::find_if(CODECS.begin(), CODECS.end(), [&](const auto& entry) {
        return codec.is<const char*>() && entry.first == codec.as<std::string_view>();
    });
    if (found == CODECS.end() || !format["sample_rate"].is<uint32_t>() ||
        !format["channels"].is<uint8_t>() || !format["bit_depth"].is<uint8_t>()) {
        return std::nullopt;
    }
    return StreamFormat{found->second, format["sample_rate"].as<uint32_t>(),
                        format["channels"].as<uint8_t>(), format["bit_depth"].as<uint8_t>()};
}

/// The pair command's method, as the REPL's pair command takes it: the code the client shows
/// when empty
std::optional<PairingOptions> pairing_method(std::string_view word) {
    if (word.empty()) {
        return PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt};
    }
    if (word == "psk") {
        return PairingOptions{PairMethod::PAIRING_PSK, std::nullopt};
    }
    if (word == "static") {
        return PairingOptions{PairMethod::STATIC_PAIRING_CODE, std::nullopt};
    }
    if (word == "digits") {
        return PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS};
    }
    if (word == "qr") {
        return PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::QR_CODE};
    }
    return std::nullopt;
}

/// A colors object, each field named as Colors names it and holding "rrggbb"; empty for none
std::optional<Colors> colors_field(JsonObjectConst object) {
    const std::array<std::pair<std::string_view, std::optional<RgbColor> Colors::*>, 6> names{{
        {"background_dark", &Colors::background_dark},
        {"background_light", &Colors::background_light},
        {"primary", &Colors::primary},
        {"accent", &Colors::accent},
        {"on_dark", &Colors::on_dark},
        {"on_light", &Colors::on_light},
    }};
    constexpr size_t HEX_DIGITS = 6;
    constexpr int HEX_BASE = 16;
    Colors colors;
    for (const JsonPairConst field : object) {
        const auto name = std::find_if(names.begin(), names.end(), [&](const auto& entry) {
            return entry.first == field.key().c_str();
        });
        if (name == names.end() || !field.value().is<const char*>()) {
            return std::nullopt;
        }
        const std::string hex = field.value().as<std::string>();
        if (hex.size() != HEX_DIGITS || !std::all_of(hex.begin(), hex.end(), [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c)) != 0;
            })) {
            return std::nullopt;
        }
        std::array<uint8_t, 3> rgb{};
        for (size_t i = 0; i < rgb.size(); ++i) {
            rgb[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, HEX_BASE));
        }
        colors.*(name->second) = RgbColor{rgb[0], rgb[1], rgb[2]};
    }
    return colors;
}

// ==============================================================================
// Commands
// ==============================================================================

/// A client command: approve, revoke, cancel_pairing, unpair
Outcome client_command(const Context& context, std::string_view command, JsonObjectConst args) {
    const std::optional<std::string> client = text_field(args, "client", MAX_FIELD_BYTES);
    if (!client) {
        return "a client is required";
    }
    SendspinServer& server = context.server;
    bool done = false;
    if (command == "approve") {
        // roles/source/v1.md "Unpaired access": the source role needs an approval of its own
        const bool source = bool_field(args, "source").value_or(false);
        done = server.approve(
            *client, source ? ApprovalScope::PLAYBACK_AND_SOURCE : ApprovalScope::PLAYBACK);
    } else if (command == "revoke") {
        done = server.revoke_approval(*client);
    } else if (command == "cancel_pairing") {
        done = server.cancel_pairing(*client);
    } else {
        done = server.unpair(*client);
    }
    return done ? Outcome() : "no such client";
}

/// A group's transport command: pause, resume, stop, next, live
Outcome transport_command(const Context& context, std::string_view command, GroupId group) {
    WebPlayback& playback = context.playback;
    if (command == "pause") {
        return playback.pause(group) ? Outcome() : Outcome("nothing to pause");
    }
    if (command == "resume") {
        return playback.resume(group) ? Outcome() : Outcome("nothing is paused");
    }
    if (command == "next") {
        return playback.next(group) ? Outcome() : Outcome("nothing to skip; the log says why");
    }
    if (command == "stop") {
        playback.stop(group);
    } else if (!playback.has_live()) {
        return "server_cli was not started with --stdin";
    } else if (!playback.live(group)) {
        return "standard input cannot play there; the log says why";
    }
    return std::nullopt;
}

/// play, queue and list: a path in the media directory
Outcome media_command(const Context& context, std::string_view command, JsonObjectConst args,
                      JsonObject data) {
    if (context.media == nullptr) {
        return "server_cli was not started with --media-dir";
    }
    const JsonVariantConst path = args["path"];
    if (!path.is<const char*>() || path.as<std::string_view>().size() > MAX_PATH_BYTES) {
        return "a path is required";
    }
    const std::string_view relative = path.as<std::string_view>();
    if (command == "list") {
        const std::optional<MediaDir::Listing> listing = context.media->list(relative);
        if (!listing) {
            return "not a directory in the media directory";
        }
        JsonArray entries = data["entries"].to<JsonArray>();
        for (const MediaDir::Entry& entry : listing->entries) {
            JsonObject item = entries.add<JsonObject>();
            item["name"] = entry.name;
            item["directory"] = entry.directory;
        }
        data["truncated"] = listing->truncated;
        return std::nullopt;
    }
    const std::optional<GroupId> group = group_field(context, args);
    if (!group) {
        return "no such group";
    }
    const std::optional<std::filesystem::path> file = context.media->resolve(relative);
    std::error_code error;
    if (!file || !std::filesystem::is_regular_file(*file, error)) {
        return "not a file in the media directory";
    }
    if (command == "queue") {
        context.playback.queue(*group, file->string());
        return std::nullopt;
    }
    return context.playback.play(*group, file->string())
               ? Outcome()
               : Outcome("the file cannot play; the log says why");
}

/// A group command other than a transport or media one
Outcome group_command(const Context& context, std::string_view command, GroupId group,
                      JsonObjectConst args) {
    SendspinServer& server = context.server;
    if (command == "volume") {
        const JsonVariantConst volume = args["volume"];
        if (!volume.is<uint32_t>() || volume.as<uint32_t>() > MAX_VOLUME) {
            return "a volume from 0 to 100 is required";
        }
        server.set_group_volume(group, volume.as<uint32_t>());
    } else if (command == "mute") {
        const std::optional<bool> muted = bool_field(args, "muted");
        if (!muted) {
            return "muted is required";
        }
        server.set_group_mute(group, *muted);
    } else if (command == "rename_group") {
        const std::optional<std::string> name =
            text_field(args, "name", config_limits::MAX_NAME_BYTES);
        if (!name || !server.rename_group(group, *name)) {
            return "a name is required";
        }
    } else if (command == "seek") {
        const JsonVariantConst seconds = args["seconds"];
        if (!seconds.is<double>() || !std::isfinite(seconds.as<double>()) ||
            seconds.as<double>() < 0.0) {
            return "a position in seconds is required";
        }
        if (!context.playback.seek(group, seconds.as<double>())) {
            return "nothing to seek; the log says why";
        }
    } else if (command == "loop") {
        const std::optional<bool> on = bool_field(args, "on");
        if (!on) {
            return "on is required";
        }
        context.playback.set_loop(group, *on);
    } else if (command == "artwork") {
        const JsonVariantConst source = args["source"];
        const std::string_view name =
            source.is<const char*>() ? source.as<std::string_view>() : std::string_view();
        if (name != "album" && name != "artist") {
            return "a source, album or artist, is required";
        }
        // An image comes by POST /artwork; this clears one
        server.set_artwork(group, name == "album" ? ArtworkSource::ALBUM : ArtworkSource::ARTIST,
                           {}, server.now_us());
    } else {
        const JsonVariantConst object = args["colors"];
        const std::optional<Colors> colors = object.is<JsonObjectConst>()
                                                 ? colors_field(object.as<JsonObjectConst>())
                                                 : std::nullopt;
        if (!colors) {
            return "colors must map background_dark, background_light, primary, accent, on_dark "
                   "or on_light to rrggbb";
        }
        server.set_colors(group, *colors, server.now_us());
    }
    return std::nullopt;
}

/// Pairing with a code or token: pair, code, token
Outcome pairing_command(const Context& context, std::string_view command, JsonObjectConst args) {
    const std::optional<std::string> client = text_field(args, "client", MAX_FIELD_BYTES);
    if (!client) {
        return "a client is required";
    }
    SendspinServer& server = context.server;
    if (command == "pair") {
        const JsonVariantConst method = args["method"];
        const std::optional<PairingOptions> options =
            method.isNull()            ? pairing_method("")
            : method.is<const char*>() ? pairing_method(method.as<std::string_view>())
                                       : std::nullopt;
        if (!options) {
            return "the method is psk, static, digits or qr, or none for the code the client shows";
        }
        return server.begin_pairing(*client, *options)
                   ? Outcome()
                   : Outcome("not a client offering that method");
    }
    const std::optional<std::string> secret =
        text_field(args, command == "code" ? "code" : "token", MAX_FIELD_BYTES);
    if (!secret) {
        return command == "code" ? "a code is required" : "a token is required";
    }
    if (command == "code") {
        return server.submit_pairing_code(*client, *secret) ? Outcome() : Outcome("no such client");
    }
    return server.submit_pairing_token(*client, *secret)
               ? Outcome()
               : Outcome("not that client's pairing token, or not a client");
}

Outcome dispatch(const Context& context, std::string_view command, JsonObjectConst args,
                 JsonObject data) {
    SendspinServer& server = context.server;
    if (command == "approve" || command == "revoke" || command == "cancel_pairing" ||
        command == "unpair") {
        return client_command(context, command, args);
    }
    if (command == "pair" || command == "code" || command == "token") {
        return pairing_command(context, command, args);
    }
    if (command == "play" || command == "queue" || command == "list") {
        return media_command(context, command, args, data);
    }
    if (command == "connect") {
        const std::optional<std::string> url = text_field(args, "url", MAX_FIELD_BYTES);
        return url && server.connect_url(*url) ? Outcome() : Outcome("not a ws:// URL");
    }
    if (command == "one_group") {
        const std::optional<bool> on = bool_field(args, "on");
        if (!on) {
            return "on is required";
        }
        if (!*on && context.one_group_fixed) {
            return "server_cli was started with --one-group";
        }
        context.one_group.store(*on);
        if (*on) {
            gather_into_server_group(server);
        }
        return std::nullopt;
    }
    if (command == "create_group") {
        const std::optional<std::string> name =
            text_field(args, "name", config_limits::MAX_NAME_BYTES);
        const std::optional<std::string> client = text_field(args, "client", MAX_FIELD_BYTES);
        if (!name || !client) {
            return "a name and a client are required";
        }
        const std::optional<GroupId> created = server.create_group(*name, *client);
        if (!created) {
            return "no such client";
        }
        data["group"] = created->value;
        return std::nullopt;
    }
    if (command == "route") {
        const std::optional<std::string> client = text_field(args, "client", MAX_FIELD_BYTES);
        const std::optional<GroupId> group = group_field(context, args);
        if (!client || !group) {
            return "a client and a group are required";
        }
        return context.playback.route(*group, *client)
                   ? Outcome()
                   : Outcome("the source cannot be routed there; the log says why");
    }
    if (command == "move" || command == "format") {
        const std::optional<std::string> client = text_field(args, "client", MAX_FIELD_BYTES);
        if (!client) {
            return "a client is required";
        }
        if (command == "move") {
            const std::optional<GroupId> group = group_field(context, args);
            return group && server.move_client(*client, *group)
                       ? Outcome()
                       : Outcome("no such client or group");
        }
        const JsonVariantConst format = args["format"];
        std::optional<StreamFormat> parsed;
        if (!format.isNull()) {
            parsed = format.is<JsonObjectConst>() ? format_field(format.as<JsonObjectConst>())
                                                  : std::nullopt;
            if (!parsed) {
                return "the format needs codec (pcm, flac or opus), sample_rate, channels and "
                       "bit_depth";
            }
        }
        return server.set_player_format(*client, parsed) ? Outcome() : Outcome("no such client");
    }
    constexpr std::array<std::string_view, 5> TRANSPORT{"pause", "resume", "stop", "next", "live"};
    constexpr std::array<std::string_view, 7> GROUP{"volume", "mute",    "rename_group", "seek",
                                                    "loop",   "artwork", "colors"};
    const bool transport =
        std::find(TRANSPORT.begin(), TRANSPORT.end(), command) != TRANSPORT.end();
    if (!transport && std::find(GROUP.begin(), GROUP.end(), command) == GROUP.end()) {
        return "unknown command";
    }
    const std::optional<GroupId> group = group_field(context, args);
    if (!group) {
        return "no such group";
    }
    return transport ? transport_command(context, command, *group)
                     : group_command(context, command, *group, args);
}

void put_format(JsonObject object, const StreamFormat& format) {
    constexpr std::array<const char*, 3> CODECS{"pcm", "flac", "opus"};
    object["codec"] = CODECS.at(static_cast<size_t>(format.codec));
    object["sample_rate"] = format.sample_rate;
    object["channels"] = format.channels;
    object["bit_depth"] = format.bit_depth;
}

std::string serialize(const JsonDocument& doc) {
    std::string text;
    serializeJson(doc, text);
    return text;
}

/// A file's name is shown if it is a WAV or FLAC file, by its extension in any case
bool playable(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".wav" || extension == ".flac";
}

}  // namespace

bool request_allowed(std::string_view origin, std::string_view host, bool loopback_only) {
    const std::string_view name = host_name(host);
    if (name.empty() || !(loopback_only ? loopback_name(name) : local_name(name))) {
        return false;
    }
    if (origin.empty()) {
        return true;
    }
    constexpr std::string_view SCHEME = "http://";
    if (!origin.starts_with(SCHEME)) {
        return false;
    }
    // Host names compare without regard to case (RFC 3986 section 3.2.2)
    const std::string_view named = origin.substr(SCHEME.size());
    return named.size() == host.size() &&
           std::equal(named.begin(), named.end(), host.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

std::optional<ArtworkTarget> parse_artwork_target(std::string_view target) {
    constexpr std::string_view PATH = "/artwork?";
    constexpr int DECIMAL_BASE = 10;
    if (!target.starts_with(PATH)) {
        return std::nullopt;
    }
    const std::string group(query_value(target, "group"));
    const std::string_view source = query_value(target, "source");
    // strtoull() alone would take a sign or leading spaces
    if (group.empty() || !std::all_of(group.begin(), group.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        return std::nullopt;
    }
    errno = 0;
    const unsigned long long id = std::strtoull(group.c_str(), nullptr, DECIMAL_BASE);
    if (errno == ERANGE || (source != "album" && source != "artist")) {
        return std::nullopt;
    }
    return ArtworkTarget{GroupId{id},
                         source == "album" ? ArtworkSource::ALBUM : ArtworkSource::ARTIST};
}

std::string valid_utf8(std::string_view text) {
    // RFC 3629 section 4: the lead byte gives the length; continuation bytes are 10xxxxxx; no
    // overlong form, surrogate or code point past U+10FFFF
    constexpr std::string_view REPLACEMENT = "\xEF\xBF\xBD";
    constexpr unsigned char CONTINUATION_MASK = 0xC0;
    constexpr unsigned char CONTINUATION = 0x80;
    constexpr uint32_t PAYLOAD_MASK = 0x3F;
    constexpr int PAYLOAD_BITS = 6;
    struct Form {
        unsigned char lead_mask;
        unsigned char lead;
        size_t length;
        uint32_t least;
    };
    constexpr std::array<Form, 4> FORMS{{
        {0x80, 0x00, 1, 0},
        {0xE0, 0xC0, 2, 0x80},
        {0xF0, 0xE0, 3, 0x800},
        {0xF8, 0xF0, 4, 0x10000},
    }};
    constexpr uint32_t SURROGATE_FIRST = 0xD800;
    constexpr uint32_t SURROGATE_LAST = 0xDFFF;
    constexpr uint32_t LAST_CODE_POINT = 0x10FFFF;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const auto form = std::find_if(FORMS.begin(), FORMS.end(), [&](const Form& entry) {
            return (lead & entry.lead_mask) == entry.lead;
        });
        bool valid = form != FORMS.end() && i + form->length <= text.size();
        if (valid) {
            uint32_t code_point = lead & static_cast<unsigned char>(~form->lead_mask);
            for (size_t k = 1; valid && k < form->length; ++k) {
                const auto next = static_cast<unsigned char>(text[i + k]);
                valid = (next & CONTINUATION_MASK) == CONTINUATION;
                code_point = (code_point << PAYLOAD_BITS) | (next & PAYLOAD_MASK);
            }
            valid = valid && code_point >= form->least && code_point <= LAST_CODE_POINT &&
                    (code_point < SURROGATE_FIRST || code_point > SURROGATE_LAST);
        }
        if (valid) {
            out.append(text.substr(i, form->length));
            i += form->length;
        } else {
            out.append(REPLACEMENT);
            ++i;
        }
    }
    return out;
}

// ==============================================================================
// MediaDir
// ==============================================================================

std::optional<MediaDir> MediaDir::open(const std::string& root) {
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(canonical, error)) {
        return std::nullopt;
    }
    return MediaDir(std::move(canonical));
}

std::optional<std::filesystem::path> MediaDir::resolve(std::string_view relative) const {
    const std::filesystem::path given(relative);
    if (relative.find('\0') != std::string_view::npos || given.has_root_path()) {
        return std::nullopt;
    }
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::canonical(this->root_ / given, error);
    if (error) {
        return std::nullopt;
    }
    // Inside the root when the root's parts begin it: canonical() resolved every ".." and link
    const auto root_end =
        std::mismatch(this->root_.begin(), this->root_.end(), canonical.begin(), canonical.end())
            .first;
    if (root_end != this->root_.end()) {
        return std::nullopt;
    }
    return canonical;
}

std::optional<MediaDir::Listing> MediaDir::list(std::string_view relative) const {
    const std::optional<std::filesystem::path> directory = this->resolve(relative);
    std::error_code error;
    if (!directory || !std::filesystem::is_directory(*directory, error)) {
        return std::nullopt;
    }
    Listing listing;
    std::filesystem::directory_iterator it(*directory, error);
    for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
        const std::string name = it->path().filename().string();
        // Hidden files, as a file manager hides them
        if (name.starts_with('.')) {
            continue;
        }
        // A link leading inside the root lists as what it leads to; one leading outside is left out
        std::error_code entry_error;
        if (it->is_symlink(entry_error) &&
            !this->resolve((std::filesystem::path(relative) / name).string())) {
            continue;
        }
        const bool is_directory = it->is_directory(entry_error);
        if (!is_directory && !playable(it->path())) {
            continue;
        }
        listing.entries.push_back({name, is_directory});
    }
    // Sorted before the cut, so the entries listed are the first by name
    std::sort(listing.entries.begin(), listing.entries.end(), [](const Entry& a, const Entry& b) {
        return a.directory != b.directory ? a.directory : a.name < b.name;
    });
    if (listing.entries.size() > MAX_LISTED_ENTRIES) {
        listing.entries.resize(MAX_LISTED_ENTRIES);
        listing.truncated = true;
    }
    return listing;
}

// ==============================================================================
// ClientWatch
// ==============================================================================

void ClientWatch::add(const TracedMessage& message) {
    if (message.client_id.empty() || message.plaintext.empty()) {
        return;
    }
    std::string_view body(reinterpret_cast<const char*>(message.plaintext.data()),
                          message.plaintext.size());
    // A binary message's first byte is its message ID; ID 0 carries JSON
    if (message.binary) {
        if (body.front() != 0) {
            return;
        }
        body.remove_prefix(1);
    }
    // Audio and time messages pass by without a parse
    const bool outbound = message.direction == TraceDirection::OUTBOUND;
    if (outbound ? body.find("\"stream/") == std::string_view::npos
                 : body.find("\"client/state\"") == std::string_view::npos) {
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body.data(), body.size()) != DeserializationError::Ok) {
        return;
    }
    const std::string_view type = doc["type"].as<std::string_view>();
    const JsonObjectConst payload = doc["payload"];
    const std::lock_guard<std::mutex> lock(this->mutex_);
    Seen& seen = this->seen_[message.client_id];
    if (outbound && type == "stream/start" && payload["player"].is<JsonObjectConst>()) {
        seen.format = format_field(payload["player"].as<JsonObjectConst>());
    } else if (outbound && type == "stream/end") {
        // messaging.md "stream/end": without roles, every stream ends
        const JsonArrayConst roles = payload["roles"];
        bool player = roles.isNull();
        for (const JsonVariantConst role : roles) {
            player = player || role.as<std::string_view>() == "player";
        }
        if (player) {
            seen.format.reset();
        }
    } else if (!outbound && type == "client/state" && payload["player"].is<JsonObjectConst>()) {
        const JsonVariantConst volume = payload["player"]["volume"];
        const JsonVariantConst muted = payload["player"]["muted"];
        if (volume.is<uint32_t>()) {
            seen.volume = volume.as<uint32_t>();
        }
        if (muted.is<bool>()) {
            seen.muted = muted.as<bool>();
        }
    }
}

std::map<std::string, ClientWatch::Seen> ClientWatch::snapshot() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->seen_;
}

// ==============================================================================
// WebCommands
// ==============================================================================

WebCommands::WebCommands(SendspinServer& server, WebPlayback& playback, const MediaDir* media,
                         const ClientWatch& watch, std::atomic<bool>& one_group,
                         bool one_group_fixed)
    : server_(server),
      playback_(playback),
      media_(media),
      watch_(watch),
      one_group_(one_group),
      one_group_fixed_(one_group_fixed) {}

std::string WebCommands::run(std::string_view text) {
    JsonDocument result;
    result["type"] = "result";
    result["id"] = nullptr;
    const auto fail = [&](const char* error) {
        result["ok"] = false;
        result["error"] = error;
        return serialize(result);
    };
    if (text.size() > MAX_COMMAND_BYTES) {
        return fail("the command is too large");
    }
    JsonDocument doc;
    if (deserializeJson(doc, text.data(), text.size()) != DeserializationError::Ok ||
        !doc.is<JsonObjectConst>()) {
        return fail("not a JSON object");
    }
    const JsonObjectConst args = doc.as<JsonObjectConst>();
    if (!args["id"].is<uint32_t>()) {
        return fail("an id from 0 to 4294967295 is required");
    }
    result["id"] = args["id"].as<uint32_t>();
    if (!args["cmd"].is<const char*>()) {
        return fail("a cmd is required");
    }
    const Context context{this->server_, this->playback_, this->media_, this->one_group_,
                          this->one_group_fixed_};
    const Outcome outcome = dispatch(context, args["cmd"].as<std::string_view>(), args,
                                     result["data"].to<JsonObject>());
    if (outcome) {
        result.remove("data");
        return fail(outcome->c_str());
    }
    result["ok"] = true;
    return serialize(result);
}

std::string WebCommands::state() const {
    JsonDocument doc;
    doc["type"] = "state";
    doc["one_group"] = this->one_group_.load();
    doc["one_group_fixed"] = this->one_group_fixed_;
    doc["loop"] = this->playback_.loop();
    doc["live"] = this->playback_.has_live();
    doc["media"] = this->media_ != nullptr;
    const std::vector<GroupInfo> groups = this->server_.groups();
    const std::map<std::string, ClientWatch::Seen> seen = this->watch_.snapshot();
    const std::optional<GroupId> server_group = this->server_.default_group();
    JsonArray clients = doc["clients"].to<JsonArray>();
    for (const ClientInfo& info : this->server_.clients()) {
        JsonObject client = clients.add<JsonObject>();
        client["client_id"] = info.client_id;
        client["name"] = info.name;
        client["url"] = info.url;
        client["address"] = info.address;
        client["connected"] = info.connected;
        client["accepted"] = info.accepted;
        client["discovered"] = info.discovered;
        client["reconnecting"] = info.reconnecting;
        client["approved"] = info.approval.has_value();
        client["unpaired_access"] = info.unpaired_access;
        client["paired"] = info.paired;
        client["pairing"] = info.pairing;
        client["credential_mismatch"] = info.credential_mismatch;
        JsonArray roles = client["roles"].to<JsonArray>();
        for (const std::string& role : info.roles) {
            roles.add(role);
        }
        JsonObject offers = client["offers"].to<JsonObject>();
        offers["psk"] = info.pairing_offers.pairing_psk;
        offers["static"] = info.pairing_offers.static_pairing_code;
        offers["digits"] = info.pairing_offers.dynamic_digits;
        offers["qr"] = info.pairing_offers.dynamic_qr_code;
        client["group"] = nullptr;
        for (const GroupInfo& group : groups) {
            if (std::find(group.client_ids.begin(), group.client_ids.end(), info.client_id) !=
                group.client_ids.end()) {
                client["group"] = group.id.value;
            }
        }
        const auto watched = seen.find(info.client_id);
        client["format"] = nullptr;
        if (watched != seen.end() && info.connected) {
            if (watched->second.format) {
                put_format(client["format"].to<JsonObject>(), *watched->second.format);
            }
            if (watched->second.volume) {
                client["volume"] = *watched->second.volume;
            }
            if (watched->second.muted) {
                client["muted"] = *watched->second.muted;
            }
        }
    }
    JsonArray listed = doc["groups"].to<JsonArray>();
    for (const GroupInfo& info : groups) {
        JsonObject group = listed.add<JsonObject>();
        group["id"] = info.id.value;
        group["name"] = info.name;
        group["server_group"] = server_group == info.id;
        group["playing"] = info.playback_state == PlaybackState::PLAYING;
        JsonArray members = group["members"].to<JsonArray>();
        for (const std::string& client_id : info.client_ids) {
            members.add(client_id);
        }
        const std::optional<WebPlaybackStatus> status = this->playback_.status(info.id);
        if (!status) {
            group["playback"] = nullptr;
            continue;
        }
        JsonObject playback = group["playback"].to<JsonObject>();
        playback["live"] = status->live;
        playback["phase"] = status->phase;
        playback["paused"] = status->paused;
        playback["latency_ms"] = status->latency_ms;
        if (!status->source.empty()) {
            playback["source"] = status->source;
        } else if (status->live) {
            playback["written"] = status->written;
            playback["dropped"] = status->dropped;
            playback["ended"] = status->ended;
        } else {
            playback["title"] = status->title;
            playback["file"] = status->file;
            playback["position_s"] = status->position_s;
            playback["duration_s"] = status->duration_s;
        }
    }
    return serialize(doc);
}

}  // namespace sendspin::server::examples
