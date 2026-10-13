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

#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "profile.h"
#include "sdkconfig.h"
#include "thread_esp.h"
#include "thread_setup.h"

#include <system_error>
#include <utility>

namespace sendspin::server {

uint32_t stack_caps(MemoryLocation location, size_t stack_bytes) {
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    // The free size, not the largest block: finding that walks the heap with interrupts off
    if (location == MemoryLocation::PREFER_EXTERNAL &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= stack_bytes) {
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
#else
    (void)location;
    (void)stack_bytes;
#endif
    return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}

namespace {

/// esp_pthread_cfg_t's "no core": ESP-IDF's pthreads take a core only in 0 ..
/// CONFIG_FREERTOS_NUMBER_OF_CORES - 1 and give any other value the project's default
constexpr int PTHREAD_NO_AFFINITY = -1;
static_assert(CONFIG_PTHREAD_TASK_CORE_DEFAULT == -1,
              "the connections' writer and reader tasks run on either core: "
              "CONFIG_PTHREAD_TASK_CORE_DEFAULT must be -1 (no affinity), as ESP-IDF's default is");

// The core thread and the dispatch thread on core 0, the group audio worker and the artwork thread
// on core 1; each stack where profile.h places it
esp_pthread_cfg_t config_of(ThreadRole role) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.inherit_cfg = false;
    MemoryLocation stack = MemoryLocation::PREFER_INTERNAL;
    switch (role) {
        case ThreadRole::CORE:
            cfg.thread_name = "ss_core";
            cfg.stack_size = 24 * 1024;
            cfg.prio = 10;
            cfg.pin_to_core = 0;
            stack = profile::CORE_STACK;
            break;
        case ThreadRole::NETWORK:
            // The host's Asio transport alone starts one; ESP-IDF's transport has tasks of its own
            break;
        case ThreadRole::DISPATCH:
            cfg.thread_name = "ss_dispatch";
            cfg.stack_size = 8 * 1024;
            cfg.prio = 5;
            cfg.pin_to_core = 0;
            stack = profile::DISPATCH_STACK;
            break;
        case ThreadRole::AUDIO_WORKER:
            // The main task's priority: a job queue that keeps the core busy, as the pipelines can,
            // then shares it with the host's loop rather than starving it into the task watchdog
            cfg.thread_name = "ss_audio";
            cfg.stack_size = 16 * 1024;
            cfg.prio = ESP_TASK_MAIN_PRIO;
            cfg.pin_to_core = 1;
            stack = profile::AUDIO_WORKER_STACK;
            break;
        case ThreadRole::ARTWORK:
            // As the audio worker's, so a burst of artwork jobs shares the core with the pipelines
            // and the host's loop
            cfg.thread_name = "ss_artwork";
            cfg.stack_size = 8 * 1024;
            cfg.prio = ESP_TASK_MAIN_PRIO;
            cfg.pin_to_core = 1;
            stack = profile::ARTWORK_STACK;
            break;
        case ThreadRole::LINK_WRITER:
            // Above the core task (10), so a write the core task started proceeds while it
            // encrypts the next message; unpinned: a pin_to_core out of range leaves the pthread
            // default, CONFIG_PTHREAD_TASK_CORE_DEFAULT, which must be no affinity (below)
            cfg.thread_name = "ss_ws_tx";
            cfg.stack_size = 3 * 1024;
            cfg.prio = 11;
            cfg.pin_to_core = PTHREAD_NO_AFFINITY;
            stack = profile::LINK_WRITER_STACK;
            break;
        case ThreadRole::LINK_READER:
            // Above the writers and the core task, so a message is stamped as soon as it is read;
            // the stack holds the dial's resolve (getaddrinfo), connect and upgrade (Sha1), then
            // the frames' reads and a close frame it sends (a peer's echoed or a protocol
            // failure's: its masked copy and a ChaCha20 block); about 1.8 KB stayed free under
            // eight dialed clients' load
            cfg.thread_name = "ss_ws_rx";
            cfg.stack_size = 3584;
            cfg.prio = 12;
            cfg.pin_to_core = PTHREAD_NO_AFFINITY;
            stack = profile::DIAL_RECEIVE_STACK;
            break;
    }
    cfg.stack_alloc_caps = stack_caps(stack, cfg.stack_size);
    return cfg;
}

}  // namespace

// esp_pthread_set_cfg() applies to every later std::thread the calling thread starts, so the
// caller's own setting (or the default, when it had none) goes back once the thread exists
std::thread start_thread(ThreadRole role, std::function<void()> body) {
    esp_pthread_cfg_t previous = esp_pthread_get_default_config();
    const bool had_config = esp_pthread_get_cfg(&previous) == ESP_OK;
    const esp_pthread_cfg_t cfg = config_of(role);
    esp_pthread_set_cfg(&cfg);
    // A failed start throws; the caller's setting goes back first
    struct Restore {
        const esp_pthread_cfg_t& previous;
        bool had_config;
        ~Restore() {
            if (had_config) {
                esp_pthread_set_cfg(&this->previous);
            } else {
                const esp_pthread_cfg_t none = esp_pthread_get_default_config();
                esp_pthread_set_cfg(&none);
            }
        }
    } restore{previous, had_config};
    if (cfg.stack_alloc_caps != (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        // PSRAM's free size can hide a fragmented heap: without a block for the stack, the thread
        // starts with it in internal RAM
        try {
            return std::thread(body);
        } catch (const std::system_error&) {
            esp_pthread_cfg_t internal = cfg;
            internal.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            esp_pthread_set_cfg(&internal);
        }
    }
    return std::thread(std::move(body));
}

// The task's handle, which every task has, pthread or not
ThreadToken current_thread_token() {
    return xTaskGetCurrentTaskHandle();
}

}  // namespace sendspin::server
