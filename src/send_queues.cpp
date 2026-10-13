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

#include "send_queues.h"

#include "fragmentation.h"

#include <span>
#include <utility>

namespace sendspin::server {

OutboundPlaintext json_plaintext(std::string_view json) {
    OutboundPlaintext message{MessageKind::BINARY, {}};
    reserve_plaintext(message.bytes, json.size() + 1);
    message.bytes.push_back(MESSAGE_ID_JSON);
    message.bytes.insert(message.bytes.end(), json.begin(), json.end());
    return message;
}

SendQueues::SendQueues(const SendQueueCaps& caps)
    : caps_(caps),
      time_(caps.time_entries, caps.time_entries, MemoryLocation::PREFER_EXTERNAL),
      control_(caps.control_entries, caps.control_entries, MemoryLocation::PREFER_EXTERNAL) {}

bool SendQueues::push_time(const ClientTime& request) {
    return this->time_.try_push(request);
}

bool SendQueues::push_control(OutboundPlaintext message) {
    return this->push({ControlKind::MESSAGE, std::move(message), {}});
}

bool SendQueues::push_activation(OutboundPlaintext message, sched::RoleSet roles) {
    return this->push({ControlKind::ACTIVATION, std::move(message), roles});
}

bool SendQueues::push_rekey(OutboundPlaintext activation, sched::RoleSet roles) {
    return this->push({ControlKind::REKEY, std::move(activation), roles});
}

bool SendQueues::push_marker() {
    if (!this->control_.empty() && this->control_.back().kind == ControlKind::MARKER) {
        return true;
    }
    if (!this->push({ControlKind::MARKER, {}, {}})) {
        return false;
    }
    ++this->markers_;
    return true;
}

bool SendQueues::push(ControlItem item) {
    const size_t bytes = item.message.bytes.size();
    if (bytes > this->caps_.control_bytes - this->control_bytes_ ||
        !this->control_.try_push(std::move(item))) {
        return false;
    }
    this->control_bytes_ += bytes;
    return true;
}

sched::PumpSnapshot SendQueues::snapshot() const {
    sched::PumpSnapshot snapshot;
    snapshot.fragment_in_progress = this->fragmenting_.has_value();
    snapshot.time_pending = !this->time_.empty();
    snapshot.control_pending = !this->control_.empty();
    return snapshot;
}

ClientTime SendQueues::take_time() {
    const ClientTime request = this->time_.front();
    this->time_.pop_front();
    return request;
}

ControlKind SendQueues::head_kind() const {
    return this->control_.front().kind;
}

sched::RoleSet SendQueues::head_roles() const {
    return this->control_.front().roles;
}

sched::RoleSet SendQueues::roles_ahead(sched::RoleSet written) const {
    for (size_t i = 1; i < this->control_.size(); ++i) {
        if (this->control_[i].kind == ControlKind::ACTIVATION ||
            this->control_[i].kind == ControlKind::REKEY) {
            return this->control_[i].roles;
        }
    }
    return written;
}

void SendQueues::pop_marker() {
    this->control_.pop_front();
    --this->markers_;
}

OutboundPlaintext SendQueues::take_rekey() {
    OutboundPlaintext activation = std::move(this->control_.front().message);
    this->control_.pop_front();
    this->control_bytes_ -= activation.bytes.size();
    ++this->control_taken_;
    return activation;
}

OutboundPlaintext SendQueues::take_control(size_t max_fragment_plaintext) {
    if (this->fragmenting_) {
        return this->next_fragment(*this->fragmenting_, max_fragment_plaintext);
    }
    OutboundPlaintext head = std::move(this->control_.front().message);
    this->control_.pop_front();
    if (head.kind == MessageKind::BINARY && needs_fragments(head.bytes.size())) {
        return this->next_fragment(this->fragmenting_.emplace(Fragmenting{std::move(head), 0}),
                                   max_fragment_plaintext);
    }
    this->control_bytes_ -= head.bytes.size();
    ++this->control_taken_;
    return head;
}

OutboundPlaintext SendQueues::next_fragment(Fragmenting& fragmenting,
                                            size_t max_fragment_plaintext) {
    const std::vector<uint8_t>& bytes = fragmenting.message.bytes;
    const std::span<const uint8_t> payload = std::span<const uint8_t>(bytes).subspan(1);
    OutboundPlaintext fragment{MessageKind::BINARY, {}};
    fragmenting.offset = write_fragment(bytes[0], payload, fragmenting.offset,
                                        max_fragment_plaintext, fragment.bytes);
    if (fragmenting.offset == payload.size()) {
        this->control_bytes_ -= bytes.size();
        ++this->control_taken_;
        this->fragmenting_.reset();
    }
    return fragment;
}

void SendQueues::clear() {
    this->time_.clear();
    this->control_.clear();
    this->fragmenting_.reset();
    this->control_bytes_ = 0;
    this->markers_ = 0;
}

}  // namespace sendspin::server
