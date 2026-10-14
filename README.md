# sendspin-server-cpp

[![CI](https://github.com/Sendspin/sendspin-server-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Sendspin/sendspin-server-cpp/actions/workflows/ci.yml)

C++ library implementing the server side of the [Sendspin synchronized audio streaming protocol](https://www.sendspin-audio.com/), so that an application (a media player, a sound server sink, a small daemon) can stream synchronized audio to Sendspin players. Targets macOS and Linux. For the client side, see [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp).

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

The library also builds as an ESP-IDF component (`cmake/esp_idf.cmake`).

### Tests

```bash
cmake -B build-asan -DSENDSPIN_SERVER_ENABLE_WERROR=ON -DSENDSPIN_SERVER_BUILD_TESTS=ON -DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

The tests also fetch GoogleTest and [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp), the client library they run against. ThreadSanitizer is a separate build: `-DSENDSPIN_SERVER_ENABLE_TSAN=ON` in place of `-DSENDSPIN_SERVER_ENABLE_SANITIZERS=ON`, run with `TSAN_OPTIONS=halt_on_error=1`.

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
