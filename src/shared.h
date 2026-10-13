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

/// @file shared.h
/// @brief The shared pointers of the transport's own objects. A libstdc++ whose default lock policy
/// is not the atomic one (the ESP-IDF riscv32 toolchain's) gives every std::shared_ptr control
/// block a mutex, a FreeRTOS semaphore in internal RAM, and takes it in weak_ptr::lock(); these
/// name the atomic policy instead. Elsewhere they are std::shared_ptr itself. The public API keeps
/// std::shared_ptr.

#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace sendspin::server {

#if defined(__GLIBCXX__) && defined(_GLIBCXX_ATOMIC_BUILTINS)

/// The toolchain's std::shared_ptr takes a mutex: name the atomic policy explicitly
inline constexpr bool SHARED_PTR_POLICY_EXPLICIT =
    __gnu_cxx::__default_lock_policy != __gnu_cxx::_S_atomic;

template <class T>
using SharedPtr =
    std::conditional_t<SHARED_PTR_POLICY_EXPLICIT, std::__shared_ptr<T, __gnu_cxx::_S_atomic>,
                       std::shared_ptr<T>>;
template <class T>
using WeakPtr = std::conditional_t<SHARED_PTR_POLICY_EXPLICIT,
                                   std::__weak_ptr<T, __gnu_cxx::_S_atomic>, std::weak_ptr<T>>;
/// @brief A class managed by SharedPtr derives from this, not std::enable_shared_from_this, whose
/// weak reference only a std::shared_ptr sets
template <class T>
using EnableSharedFromThis =
    std::conditional_t<SHARED_PTR_POLICY_EXPLICIT,
                       std::__enable_shared_from_this<T, __gnu_cxx::_S_atomic>,
                       std::enable_shared_from_this<T>>;

template <class T, class... Args>
SharedPtr<T> make_shared_ptr(Args&&... args) {
    if constexpr (SHARED_PTR_POLICY_EXPLICIT) {
        return std::__make_shared<T, __gnu_cxx::_S_atomic>(std::forward<Args>(args)...);
    } else {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
}

template <class T, class Alloc, class... Args>
SharedPtr<T> allocate_shared_ptr(const Alloc& alloc, Args&&... args) {
    if constexpr (SHARED_PTR_POLICY_EXPLICIT) {
        return std::__allocate_shared<T, __gnu_cxx::_S_atomic>(alloc, std::forward<Args>(args)...);
    } else {
        return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
    }
}

#else

template <class T>
using SharedPtr = std::shared_ptr<T>;
template <class T>
using WeakPtr = std::weak_ptr<T>;
template <class T>
using EnableSharedFromThis = std::enable_shared_from_this<T>;

template <class T, class... Args>
SharedPtr<T> make_shared_ptr(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

template <class T, class Alloc, class... Args>
SharedPtr<T> allocate_shared_ptr(const Alloc& alloc, Args&&... args) {
    return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
}

#endif

}  // namespace sendspin::server
