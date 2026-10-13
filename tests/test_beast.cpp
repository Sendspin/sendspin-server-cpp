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

/// @file test_beast.cpp
/// @brief Boost.Asio and Boost.Beast link from the library's separately compiled translation unit.

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <gtest/gtest.h>

#if !defined(BOOST_ASIO_SEPARATE_COMPILATION) || !defined(BOOST_BEAST_SEPARATE_COMPILATION)
#error "Asio and Beast must come from the library's separately compiled translation unit"
#endif

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;

TEST(Beast, IoContextRunsAPostedHandler) {
    asio::io_context io;
    bool ran = false;
    asio::post(io, [&ran] { ran = true; });
    EXPECT_EQ(io.run(), 1U);
    EXPECT_TRUE(ran);
}

TEST(Beast, WebSocketStreamTakesTheTransportSettings) {
    asio::io_context io;
    beast::websocket::stream<beast::tcp_stream> ws(io);
    ws.auto_fragment(false);
    ws.read_message_max(64 * 1024);
    ws.write_buffer_bytes(64 * 1024);
    EXPECT_FALSE(ws.auto_fragment());
    EXPECT_EQ(ws.read_message_max(), 64U * 1024U);
    EXPECT_EQ(ws.write_buffer_bytes(), 64U * 1024U);
    EXPECT_FALSE(ws.is_open());
    // Defined in the separately compiled Beast sources
    EXPECT_EQ(beast::http::to_string(beast::http::verb::get), "GET");
    EXPECT_EQ(beast::http::string_to_verb("POST"), beast::http::verb::post);
}

}  // namespace
