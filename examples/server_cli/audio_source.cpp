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

#include "audio_source.h"

#include "flac_file.h"
#include "wav_reader.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace sendspin::server::examples {

size_t frame_bytes(const InputFormat& format) {
    switch (format.sample_format) {
        case SampleFormat::S16:
            return size_t{2} * format.channels;
        case SampleFormat::S24:
            return size_t{3} * format.channels;
        case SampleFormat::F32:
            return size_t{4} * format.channels;
    }
    return 0;
}

OpenedAudio open_audio_file(const std::string& path) {
    std::array<char, 4> magic{};
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {nullptr, "cannot read " + path};
    }
    const size_t got = std::fread(magic.data(), 1, magic.size(), file);
    std::fclose(file);
    if (got == magic.size() && std::memcmp(magic.data(), "fLaC", magic.size()) == 0) {
        return open_flac_file(path);
    }
    if (got == magic.size() && std::memcmp(magic.data(), "RIFF", magic.size()) == 0) {
        return open_wav_file(path);
    }
    return {nullptr, "not a WAV or native FLAC file"};
}

}  // namespace sendspin::server::examples
