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

/// @file log.h
/// @brief The server's log: SS_LOGE, SS_LOGW and SS_LOGI format a line where it happens, when its
/// level is logged, and the dispatch thread hands it to ServerConfig::log

#pragma once

#include "dispatcher.h"
#include "sendspin/server/config.h"

#include <memory>
#include <string>
#include <string_view>

namespace sendspin::server {

/// @brief printf-style formatting into a string
[[gnu::format(printf, 1, 2)]] std::string log_format(const char* format, ...);

/// @brief text with each control character replaced by '?', for a peer's text in a line, which
/// could otherwise forge one
std::string printable(std::string_view text);

/// @brief url without its userinfo, query or fragment, through printable(): a URL to log
std::string url_for_log(std::string_view url);

/// @brief One run's log: the host's sink and level. Any thread, and never waits: the sink is called
/// on the dispatch thread, at once from there and through a posted task from any other, so core
/// never waits on it, it needs no lock and a line keeps its place among the listener calls. A
/// line posted once the dispatch thread is finishing is dropped. A copy must not outlive the
/// dispatcher.
class Logger {
public:
    /// @param sink empty for standard error
    Logger(Dispatcher& dispatcher, const LogFn& sink, LogLevel level);

    /// @param level never LogLevel::OFF
    bool enabled(LogLevel level) const {
        return level <= this->level_;
    }

    void log(LogLevel level, std::string line) const;

private:
    Dispatcher& dispatcher_;
    /// Shared with the posted lines, which may run after the logger is gone
    std::shared_ptr<const LogFn> sink_;
    LogLevel level_;
};

}  // namespace sendspin::server

/// Formats and logs a line at level through logger, only when the level is logged
#define SS_LOG_AT(logger, level, ...)                                           \
    do {                                                                        \
        if ((logger).enabled(level)) {                                          \
            (logger).log((level), ::sendspin::server::log_format(__VA_ARGS__)); \
        }                                                                       \
    } while (false)

#define SS_LOGE(logger, ...) SS_LOG_AT(logger, ::sendspin::server::LogLevel::ERROR, __VA_ARGS__)
#define SS_LOGW(logger, ...) SS_LOG_AT(logger, ::sendspin::server::LogLevel::WARN, __VA_ARGS__)
#define SS_LOGI(logger, ...) SS_LOG_AT(logger, ::sendspin::server::LogLevel::INFO, __VA_ARGS__)
