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

#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <utility>

namespace sendspin::server {

namespace {

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR:
            return "error";
        case LogLevel::WARN:
            return "warning";
        case LogLevel::INFO:
            return "info";
        case LogLevel::OFF:
            break;
    }
    return "";
}

void log_to_stderr(LogLevel level, std::string_view line) {
    std::fprintf(stderr, "sendspin-server %s: %.*s\n", level_name(level),
                 static_cast<int>(line.size()), line.data());
}

}  // namespace

std::string log_format(const char* format, ...) {
    // NOLINTBEGIN(cppcoreguidelines-init-variables): va_start() and va_copy() initialize them
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    // NOLINTEND(cppcoreguidelines-init-variables)
    const int size = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    std::string line;
    if (size > 0) {
        line.resize(static_cast<size_t>(size) + 1);
        std::vsnprintf(line.data(), line.size(), format, args);
        line.pop_back();
    }
    va_end(args);
    return line;
}

std::string printable(std::string_view text) {
    std::string out(text);
    constexpr unsigned char FIRST_PRINTABLE = 0x20;
    constexpr unsigned char DELETE = 0x7f;
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < FIRST_PRINTABLE || byte == DELETE) {
            c = '?';
        }
    }
    return out;
}

std::string url_for_log(std::string_view url) {
    const size_t scheme = url.find("://");
    const size_t authority = scheme == std::string_view::npos ? 0 : scheme + 3;
    std::string out(url.substr(0, authority));
    const std::string_view rest = url.substr(authority);
    const size_t path = rest.find_first_of("/?#");
    std::string_view host = rest.substr(0, path);
    const size_t at = host.rfind('@');
    if (at != std::string_view::npos) {
        host.remove_prefix(at + 1);
    }
    out += host;
    if (path != std::string_view::npos) {
        const std::string_view tail = rest.substr(path);
        out += tail.substr(0, tail.find_first_of("?#"));
    }
    return printable(out);
}

Logger::Logger(Dispatcher& dispatcher, const LogFn& sink, LogLevel level)
    : dispatcher_(dispatcher),
      sink_(std::make_shared<const LogFn>(sink ? sink : LogFn(log_to_stderr))),
      level_(level) {}

void Logger::log(LogLevel level, std::string line) const {
    if (this->dispatcher_.on_thread()) {
        (*this->sink_)(level, line);
        return;
    }
    this->dispatcher_.post(
        [sink = this->sink_, level, line = std::move(line)] { (*sink)(level, line); });
}

}  // namespace sendspin::server
