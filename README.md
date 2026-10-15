# sendspin-server-cpp

[![CI](https://github.com/Sendspin/sendspin-server-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Sendspin/sendspin-server-cpp/actions/workflows/ci.yml)

C++ library implementing the server side of the [Sendspin synchronized audio streaming protocol](https://www.sendspin-audio.com/), so that an application (a media player, a sound server sink, a small daemon) can stream synchronized audio to Sendspin players. Targets macOS and Linux, and builds as an ESP-IDF component. For the client side, see [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp).

## Status

Early development. The public API changes between releases without deprecation, so pin a release or commit if you build on it.

## Features

- `SendspinServer` (`include/sendspin/server/server.h`): advertises itself and browses for clients over mDNS, accepts clients and dials them by URL, keeps every client time-synced and reconnects the ones that drop
- Mandatory Noise KKpsk2 encryption on every connection; pairing by Pairing PSK token, static code or the code a client shows (CPace), and operator approval of clients with unpaired access
- Groups the host creates, renames and moves clients between, each playing its own input
- Host PCM input, buffered (a file or decoder) or live (a capture device, whose raised latency comes back down inaudibly), streamed to each player as PCM, FLAC or Opus (off by default, `ServerConfig::enable_opus`) in its chosen format, resampled and channel-mixed per player, with in-place format switches
- Controller, metadata, artwork (JPEG or PNG, fitted to each channel), color and visualizer roles
- Source role: a client's captured audio routed into a group's live input, played a fixed latency after its capture, the source's clock drift corrected (ASRC on a host, single frames dropped or repeated on ESP-IDF)
- `examples/server_cli`: a command-line and browser-controlled host on the public API

## Usage Example

A host that plays a 440 Hz tone to every client that enables unpaired access. `include/sendspin/server/server.h` documents each call and the threading contract, and `examples/server_cli` is a complete host.

```cpp
#include <sendspin/server/server.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

using namespace sendspin::server;

// Holds the server key and the client records. A real host stores them on disk, so that clients
// see the same server across runs.
class MemoryProvider : public PersistenceProvider {
public:
    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override {
        auto it = blobs_.find(key);
        if (it == blobs_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        blobs_[key].assign(data, data + len);
        return true;
    }

private:
    std::map<std::string, std::vector<uint8_t>> blobs_;
};

// Called on the server's dispatch thread, one call at a time
class PrintingListener : public ServerListener {
public:
    void on_client_connected(const ClientInfo& client) override {
        std::printf("connected: %s\n", client.name.c_str());
    }

    void on_pairing_required(const ClientInfo& client) override {
        std::printf("needs pairing: %s\n", client.name.c_str());
    }
};

int main() {
    MemoryProvider provider;
    PrintingListener listener;
    SendspinServer server;

    ServerConfig config;
    config.name = "Living Room";
    // Plays to every client that enables unpaired access; otherwise approve() or pair each client
    config.approve_all_clients = true;
    if (server.start(config, &provider, &listener) != StartResult::OK) {
        return 1;
    }

    // On a host the server advertises itself over mDNS and dials the clients it finds; every
    // approved client joins the server's group
    constexpr uint32_t RATE = 48000;
    InputOptions options{.format = {.sample_rate = RATE, .channels = 2}};
    OpenedInput opened = server.open_input(*server.default_group(), options);
    if (opened.result != OpenInputResult::OK) {
        return 1;
    }
    GroupInput& input = *opened.input;

    // A buffered input takes PCM as far ahead as its players can hold: write until it refuses,
    // then wait for room
    std::vector<int16_t> block(RATE / 50 * 2);
    for (uint64_t pos = 0;;) {
        for (size_t i = 0; i < block.size() / 2; ++i) {
            double t = static_cast<double>(pos + i) / RATE;
            auto s = static_cast<int16_t>(3000 * std::sin(2 * std::numbers::pi * 440 * t));
            block[2 * i] = block[2 * i + 1] = s;
        }
        size_t done = 0;
        while (done < block.size() / 2) {
            WriteResult result = input.write(block.data() + 2 * done, block.size() / 2 - done);
            done += result.frames;
            if (result.state == InputState::FULL) {
                input.wait_writable(100'000);
            } else if (result.state != InputState::OPEN) {
                // The group's last member left it (STOP_LATCHED), or the server closed the input
                return 0;
            }
        }
        pos += done;
    }
}
```

### Host (CMake)

Consume the library as a subproject, here through FetchContent (`tests/consumer/` is such a project); the requirements under [Host (macOS/Linux)](#host-macoslinux) apply:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_host LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(
    sendspin_server_cpp
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-server-cpp.git
    GIT_TAG        main  # pin a commit
)
FetchContent_MakeAvailable(sendspin_server_cpp)

add_executable(my_host main.cpp)
target_link_libraries(my_host PRIVATE sendspin_server_cpp::sendspin_server_cpp)
```

### ESP-IDF

The library is an ESP-IDF component (`cmake/esp_idf.cmake`, with the repository's `Kconfig`) and requires ESP-IDF 5.5 or later. The repository's root is the component, named after its directory, or a wrapper component includes `cmake/esp_idf.cmake` and sources the root's `Kconfig`, fixing the name whatever the checkout's directory; `tests/esp_idf/` is a minimal project that builds it the second way, as `components/sendspin_server`. The component builds with the embedded profile's limits (`src/profile.h`), without Opus and without an mDNS backend: supply a `DiscoveryProvider` in `ServerConfig::discovery` (`include/sendspin/server/discovery.h`), or dial clients with `connect_url()`.

The project supplies the component's dependencies and settings:

- `esphome/noise-c` (which brings `esphome/libsodium`) and `espressif/esp-dsp` from the component registry, in a component's `idf_component.yml`, at the versions in `tests/esp_idf/main/idf_component.yml`
- micro-flac at its v0.3.0 tag with its submodules, in a component directory named `micro-flac`, and `third_party/micro-resampler`, both in `EXTRA_COMPONENT_DIRS`
- ArduinoJson's source tree at v7.4.1 (`cmake/dependencies.cmake`'s pin), named by `SENDSPIN_SERVER_ARDUINOJSON_SOURCE_DIR` before `project()` or in the environment
- `CONFIG_COMPILER_CXX_EXCEPTIONS`, and a `CONFIG_DSP_MAX_FFT_SIZE` of at least 4096 (`CONFIG_DSP_MAX_FFT_SIZE_4096`)
- `CONFIG_PTHREAD_TASK_CORE_DEFAULT` left at ESP-IDF's default of -1 (no core affinity), or the build fails
- `CONFIG_LWIP_MAX_SOCKETS` and `CONFIG_LWIP_MAX_ACTIVE_TCP` raised from ESP-IDF's defaults, as below

`CONFIG_LWIP_MAX_SOCKETS` must be raised from ESP-IDF's default of 10: the listener's httpd takes a socket for each accepted connection in its handshake and each activated one, plus three of its own (19), and a build with fewer fails; the dials and mDNS need more, as `tests/esp_idf/sdkconfig.defaults` allows. `CONFIG_LWIP_MAX_ACTIVE_TCP` must be raised from 16 too: those sessions, the connections waiting in httpd's backlog (as many as accepted connections in their handshake) and the dials in their handshake (28) must fit, or the build fails, and each activated dialed connection takes one more.

## Documentation

- **[Internals](docs/internals.md)**: threads, cross-thread channels, a connection's lifecycle and a group's playback
- **[Conventions](docs/conventions.md)**: the design standards the code is held to

## Build

### Host (macOS/Linux)

Requires CMake 3.25 or later, a C++20 compiler and OpenSSL 3. On Linux, mDNS discovery uses avahi-client through pkg-config and `avahi-daemon` at run time; macOS uses its own dns_sd.

```bash
# Ubuntu 24.04 (22.04's cmake is older than 3.25)
sudo apt-get install ca-certificates cmake g++ git libssl-dev pkg-config libavahi-client-dev
# macOS
brew install cmake openssl@3

cmake -B build
cmake --build build
```

This builds the library (target `sendspin_server_cpp`, alias `sendspin_server_cpp::sendspin_server_cpp`) and the `server_cli` example (`-DSENDSPIN_SERVER_BUILD_EXAMPLES=OFF` skips it). Options: `-DSENDSPIN_SERVER_ENABLE_DISCOVERY=OFF` builds without mDNS, `-DSENDSPIN_SERVER_ENABLE_OPUS=OFF` without the Opus encoder and decoder, and `-DSENDSPIN_SERVER_ENABLE_WERROR=ON` makes warnings in this project's targets errors, as CI builds.

Dependencies (fetched automatically via CMake FetchContent at pinned versions): Boost.Beast and Boost.Asio (a system Boost 1.90 or later is used when installed), ArduinoJson, noise-c, micro-flac, micro-opus and pffft. micro-resampler and stb are vendored in `third_party/`; `NOTICES.md` lists every dependency's license.

The library also builds as an ESP-IDF component; see [ESP-IDF](#esp-idf).

### Tests

```bash
cmake -B build-asan -DSENDSPIN_SERVER_ENABLE_WERROR=ON -DSENDSPIN_SERVER_BUILD_TESTS=ON -DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

The tests also fetch GoogleTest and [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp), the client library they run against. ThreadSanitizer is a separate build: `-DSENDSPIN_SERVER_ENABLE_TSAN=ON` in place of `-DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON`, run with `TSAN_OPTIONS=halt_on_error=1`.

The tests whose assertions depend on players keeping up with real time carry the CTest label `realtime` and can miss their deadlines on a loaded machine. To run the rest in parallel and those on their own:

```bash
ctest --test-dir build-asan --output-on-failure -LE realtime -j 8
ctest --test-dir build-asan --output-on-failure -L realtime
```

The soak test (`tests/soak.cpp`, target `sendspin_server_cpp_soak`) runs a server with real players through a seeded random script and checks what each received. It is skipped unless `SENDSPIN_SERVER_SOAK_SECONDS` is set; `SENDSPIN_SERVER_SOAK_SEED` picks the script:

```bash
SENDSPIN_SERVER_SOAK_SECONDS=600 ctest --test-dir build-asan -L soak -V
```

## Examples

### server_cli

`examples/server_cli` is a command-line host on the public API. It keeps the server key and client records in the state directory you give it, so clients see the same server across runs:

```bash
./build/examples/server_cli [--approve-all] ~/.sendspin-server-cpp "Living Room Server"
```

It advertises the server and dials every client it finds over mDNS. `--approve-all` plays to every client that enables unpaired access; without it, `approve <client>` approves one, and a client without unpaired access is paired with `pair <client>`. `--opus` streams Opus to the players that list it. `--help` lists the options, and `help` the commands; the main ones:

| Command | Does |
| --- | --- |
| `connect <ws-url>` | Dials a client, such as `connect ws://192.168.1.20:8928/sendspin` |
| `clients`, `groups` | Lists the clients and the groups |
| `approve <client> [source]`, `revoke <client>` | Approves a client with unpaired access, or revokes that |
| `pair <client> [psk\|static\|digits\|qr]`, `code <client> <code>` | Pairs a client and enters the code it shows |
| `group <name> <client>`, `move <client> <group>`, `use <group>` | Creates a group, moves a client, selects the group the commands below act on |
| `play <file>`, `queue <file>`, `loop on\|off` | Plays or queues a WAV or FLAC file, gaplessly |
| `pause`, `resume`, `seek <seconds>`, `stop` | Transport control |
| `art <file>\|off`, `colors ...\|off`, `volume <0-100>`, `mute on\|off` | Artwork, colors, volume |
| `live`, `route <client>` | Plays standard input (`--stdin`), or a client's source, into the group |
| `trace on\|off` | Prints every message sent and received |
| `quit` | Stops the server and exits |

To play to the sendspin-cpp `basic_client` example, start it with unpaired access enabled (`basic_client -a`), then:

```text
./build/examples/server_cli --approve-all ~/.sendspin-server-cpp
play music.wav
```

`--web <[address:]port>` serves a browser dashboard that does what the commands do, and `--media-dir <dir>` gives it a directory of files to play. A bare port binds 127.0.0.1; the dashboard has no password, so anyone who reaches the port controls the server. `examples/server_cli/web_protocol.md` describes its messages.

```bash
./build/examples/server_cli --approve-all --web 8080 --media-dir ~/Music ~/.sendspin-server-cpp
```

`--stdin <rate>:<channels>:<s16|s24|f32>` plays raw little-endian PCM from standard input live:

```bash
ffmpeg -re -i music.flac -f s16le -ac 2 -ar 48000 - | ./build/examples/server_cli --approve-all --stdin 48000:2:s16 ~/.sendspin-server-cpp
```

## License

Apache 2.0
