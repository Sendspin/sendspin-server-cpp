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

/// @file stb_implementation.cpp
/// @brief The one translation unit that compiles the vendored stb_image, stb_image_resize2 and
/// stb_image_write (third_party/stb/VENDORED.md); their configuration macros come from the
/// sendspin_server_cpp_stb target, and stb_image allocates through stb_budget.h

#include "stb_budget.h"

// stb_image allocates within the calling thread's StbBudget
#define STBI_MALLOC(size) ::sendspin::server::stb_budget_malloc(size)
#define STBI_REALLOC(block, size) ::sendspin::server::stb_budget_realloc(block, size)
#define STBI_FREE(block) ::sendspin::server::stb_budget_free(block)

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
