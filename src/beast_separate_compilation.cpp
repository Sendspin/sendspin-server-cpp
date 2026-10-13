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

/// @file beast_separate_compilation.cpp
/// @brief Compiles Boost.Asio and Boost.Beast once for the library
/// (BOOST_ASIO_SEPARATE_COMPILATION and BOOST_BEAST_SEPARATE_COMPILATION, set by the
/// sendspin_server_cpp_beast target)

#include <boost/asio/impl/src.hpp>
#include <boost/beast/src.hpp>
