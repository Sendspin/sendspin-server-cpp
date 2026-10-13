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

#include "trust_policy.h"

namespace sendspin::server {

HandshakePsk select_handshake_psk(const Psk* long_term, const Psk* pairing) {
    if (long_term != nullptr) {
        return {PskCategory::LONG_TERM, *long_term};
    }
    if (pairing != nullptr) {
        return {PskCategory::PAIRING, *pairing};
    }
    // connection.md "Pre-Shared Key": the Sentinel whenever no other PSK applies
    return {};
}

bool activity_set_allowed(PskCategory matched, Activities activities, bool unpaired_access) {
    if (matched == PskCategory::LONG_TERM) {
        return !activities.pairing;
    }
    return !activities.playback || unpaired_access;
}

bool playback_capable(PskCategory matched, Activities activities, bool unpaired_access) {
    activities.playback = true;
    return activity_set_allowed(matched, activities, unpaired_access);
}

}  // namespace sendspin::server
