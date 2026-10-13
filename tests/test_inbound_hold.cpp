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

/// @file test_inbound_hold.cpp
/// @brief A connection's message held for the inbound ring: who may push it and when, the buffer
/// staying the receive task's, a pause ended only by the release of its own message, a resume
/// during a refused push not lost, and the close raised after the last push. The interleavings
/// are forced through the push callback, run on threads of their own.

#include "inbound_hold.h"
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::CloseReason;
using sendspin::server::InboundHold;
using sendspin::server::MessageKind;
using Result = InboundHold::Result;

constexpr size_t KEPT_BYTES = 16;

/// What the hold did, in order, and how the test answers its pushes
struct Recorder {
    std::mutex mutex;
    std::vector<std::string> log;
    /// Answers each push in turn; an empty list accepts
    std::vector<std::function<bool(std::span<const uint8_t>)>> answers;
    size_t pushes{0};
    size_t parks{0};

    bool push(std::span<const uint8_t> payload) {
        std::function<bool(std::span<const uint8_t>)> answer;
        {
            const std::lock_guard<std::mutex> lock(this->mutex);
            const size_t turn = this->pushes++;
            if (turn < this->answers.size()) {
                answer = this->answers[turn];
            }
        }
        const bool taken = !answer || answer(payload);
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->log.push_back(taken ? "push " + std::string(payload.begin(), payload.end())
                                  : "refused");
        return taken;
    }

    void note(const std::string& entry) {
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->log.push_back(entry);
    }
};

InboundHold make_hold(Recorder& recorder) {
    return InboundHold([&recorder](MessageKind /*kind*/, std::span<const uint8_t> payload,
                                   int64_t /*rx_us*/) { return recorder.push(payload); },
                       [&recorder] {
                           {
                               const std::lock_guard<std::mutex> lock(recorder.mutex);
                               ++recorder.parks;
                           }
                           recorder.note("park");
                       },
                       [&recorder](CloseReason /*reason*/) { recorder.note("closed"); },
                       KEPT_BYTES);
}

/// The receive task's part: assembles text in the buffer and offers it
InboundHold::Outcome receive(InboundHold& hold, const std::string& text) {
    sendspin::server::ExternalBytes* buffer = hold.begin_message();
    if (buffer == nullptr) {
        return {Result::DROPPED, 0, nullptr};
    }
    buffer->assign(text.begin(), text.end());
    return hold.offer(MessageKind::TEXT, text.size(), 0);
}

bool refuse(std::span<const uint8_t> /*payload*/) {
    return false;
}

TEST(InboundHold, HoldsAMessageUntilStartedAndPushesItThen) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    const InboundHold::Outcome offered = receive(hold, "hello");
    EXPECT_EQ(offered.result, Result::HELD);
    EXPECT_EQ(recorder.pushes, 0U);

    hold.start();
    const InboundHold::Outcome resumed = hold.resume();
    EXPECT_EQ(resumed.result, Result::PUSHED);
    EXPECT_EQ(resumed.generation, offered.generation);
    EXPECT_TRUE(hold.released(offered.generation));
    EXPECT_EQ(recorder.log, std::vector<std::string>({"push hello"}));
}

// A release on another thread reads the buffer only while it pushes and leaves it as it was: the
// receive task alone empties or frees it, at its next message
TEST(InboundHold, AReleaseOnAnotherThreadLeavesTheBufferToTheReceiveTask) {
    Recorder recorder;
    recorder.answers = {refuse, refuse};
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    const std::string large(4 * KEPT_BYTES, 'x');
    ASSERT_EQ(receive(hold, large).result, Result::HELD);
    const uint8_t* data = hold.message().data();
    const size_t capacity = hold.message().capacity();

    InboundHold::Outcome resumed{};
    std::thread core([&] { resumed = hold.resume(); });
    core.join();
    EXPECT_EQ(resumed.result, Result::PUSHED);
    EXPECT_EQ(recorder.log.back(), "push " + large);
    EXPECT_EQ(hold.message().data(), data);
    EXPECT_EQ(hold.message().capacity(), capacity);
    EXPECT_EQ(std::string(hold.message().begin(), hold.message().end()), large);

    // The next message frees the large buffer before it is assembled
    hold.begin_message();
    EXPECT_LE(hold.message().capacity(), KEPT_BYTES);
}

// A resume that comes while the receive task's own push is in flight finds nothing to do, and the
// receive task's offer is the one that pushes
TEST(InboundHold, AResumeDuringTheReceiveTasksPushDoesNothing) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    InboundHold::Outcome resumed{Result::HELD, 1, nullptr};
    recorder.answers = {[&](std::span<const uint8_t> /*payload*/) {
        std::thread core([&] { resumed = hold.resume(); });
        core.join();
        return true;
    }};
    EXPECT_EQ(receive(hold, "a").result, Result::PUSHED);
    EXPECT_EQ(resumed.result, Result::NOTHING);
    EXPECT_EQ(resumed.unpause, nullptr);
    EXPECT_EQ(recorder.pushes, 1U);
}

// A resume that finds nothing held (a stale park, a duplicate) ends no pause: only the release of
// the paused message does
TEST(InboundHold, OnlyTheReleaseOfItsOwnMessageEndsAPause) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    int token = 0;

    // A stale resume with nothing held
    EXPECT_EQ(hold.resume().result, Result::NOTHING);

    // Refused at the offer and its second try, and at the stale resume's two
    recorder.answers = {refuse, refuse, refuse, refuse};
    const InboundHold::Outcome offered = receive(hold, "b");
    ASSERT_EQ(offered.result, Result::HELD);
    // A resume that is refused too, before the receive task pauses
    std::thread core([&] { EXPECT_EQ(hold.resume().unpause, nullptr); });
    core.join();
    EXPECT_TRUE(hold.pause(offered.generation, &token));

    // Another stale resume, now refused no more, releases it and returns its pause
    InboundHold::Outcome released{};
    std::thread room([&] { released = hold.resume(); });
    room.join();
    EXPECT_EQ(released.result, Result::PUSHED);
    EXPECT_EQ(released.unpause, &token);
    EXPECT_EQ(hold.resume().result, Result::NOTHING);
}

// A resume that comes while another task is pushing the paused message ends no pause: the message
// is not released, and the pause stays recorded for its release
TEST(InboundHold, AResumeDuringAnotherResumesPushEndsNoPause) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    int token = 0;
    recorder.answers = {refuse, refuse};
    const InboundHold::Outcome offered = receive(hold, "h");
    ASSERT_EQ(offered.result, Result::HELD);
    ASSERT_TRUE(hold.pause(offered.generation, &token));

    InboundHold::Outcome stale{Result::HELD, 1, nullptr};
    recorder.answers.push_back([&](std::span<const uint8_t> /*payload*/) {
        std::thread other([&] { stale = hold.resume(); });
        other.join();
        return false;
    });
    recorder.answers.push_back(refuse);
    recorder.answers.push_back(refuse);
    recorder.answers.push_back(refuse);
    InboundHold::Outcome resumed{};
    std::thread core([&] { resumed = hold.resume(); });
    core.join();
    EXPECT_EQ(stale.result, Result::NOTHING);
    EXPECT_EQ(stale.unpause, nullptr);
    EXPECT_EQ(resumed.result, Result::HELD);
    EXPECT_FALSE(hold.released(offered.generation));
    EXPECT_EQ(hold.take_pause(), &token);
}

// A message released before the receive task records its pause: the pause is the receive task's
// to end
TEST(InboundHold, APauseRecordedAfterItsReleaseIsRefused) {
    Recorder recorder;
    recorder.answers = {refuse, refuse};
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    int token = 0;
    const InboundHold::Outcome offered = receive(hold, "c");
    ASSERT_EQ(offered.result, Result::HELD);
    std::thread core([&] { EXPECT_EQ(hold.resume().result, Result::PUSHED); });
    core.join();
    EXPECT_FALSE(hold.pause(offered.generation, &token));
    EXPECT_EQ(hold.take_pause(), nullptr);
}

// The ring's room comes while the receive task's second push is being refused: the resume it
// brings is not lost, and the receive task offers once more instead of holding with nobody to
// resume it
TEST(InboundHold, RoomDuringARefusedPushIsNotLost) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    InboundHold::Outcome resumed{Result::HELD, 1, nullptr};
    recorder.answers = {refuse, [&](std::span<const uint8_t> /*payload*/) {
                            std::thread core([&] { resumed = hold.resume(); });
                            core.join();
                            return false;
                        }};
    const InboundHold::Outcome offered = receive(hold, "d");
    EXPECT_EQ(offered.result, Result::PUSHED);
    EXPECT_EQ(resumed.result, Result::NOTHING);
    EXPECT_EQ(recorder.pushes, 3U);
    EXPECT_EQ(recorder.parks, 1U);
}

// One park until the resume it brings: a held message refused again does not park twice
TEST(InboundHold, ParksOnceUntilResumed) {
    Recorder recorder;
    recorder.answers = {refuse, refuse, refuse, refuse, refuse, refuse};
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    ASSERT_EQ(receive(hold, "e").result, Result::HELD);
    EXPECT_EQ(recorder.parks, 1U);
    ASSERT_EQ(hold.resume().result, Result::HELD);
    EXPECT_EQ(recorder.parks, 2U);
    EXPECT_EQ(hold.resume().result, Result::HELD);
    EXPECT_EQ(recorder.parks, 3U);
}

TEST(InboundHold, RaisesTheCloseAfterTheHeldMessagesPush) {
    Recorder recorder;
    recorder.answers = {refuse, refuse};
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    ASSERT_EQ(receive(hold, "goodbye").result, Result::HELD);
    hold.report_closed(CloseReason::PEER_CLOSED);
    EXPECT_EQ(recorder.log.back(), "refused");
    ASSERT_EQ(hold.resume().result, Result::PUSHED);
    EXPECT_EQ(recorder.log,
              std::vector<std::string>({"refused", "park", "refused", "push goodbye", "closed"}));
}

// The close a writer reports while the receive task's push is in flight comes after that push: a
// client/goodbye being pushed is not overtaken
TEST(InboundHold, RaisesACloseReportedDuringAPushAfterIt) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    recorder.answers = {[&](std::span<const uint8_t> /*payload*/) {
        std::thread writer([&] { hold.report_closed(CloseReason::PEER_CLOSED); });
        writer.join();
        return true;
    }};
    ASSERT_EQ(receive(hold, "goodbye").result, Result::PUSHED);
    EXPECT_EQ(recorder.log, std::vector<std::string>({"push goodbye", "closed"}));
}

// A close reported after a drop, the dropped message not yet released, waits for its release
TEST(InboundHold, RaisesACloseAfterADropOnceTheMessageIsReleased) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    ASSERT_EQ(receive(hold, "i").result, Result::HELD);
    hold.drop();
    hold.report_closed(CloseReason::FAILED);
    EXPECT_TRUE(recorder.log.empty());
    EXPECT_EQ(hold.resume().result, Result::DROPPED);
    EXPECT_EQ(recorder.log, std::vector<std::string>({"closed"}));
}

// A receive task that receives while its message is held (its pause failed) gets no buffer, and
// another offer does not touch the held one
TEST(InboundHold, RefusesTheBufferAndAnOfferWhileAMessageIsHeld) {
    Recorder recorder;
    recorder.answers = {refuse, refuse};
    InboundHold hold = make_hold(recorder);
    hold.bind();
    hold.start();
    const InboundHold::Outcome offered = receive(hold, "j");
    ASSERT_EQ(offered.result, Result::HELD);
    EXPECT_EQ(hold.begin_message(), nullptr);
    EXPECT_EQ(hold.offer(MessageKind::TEXT, 0, 0).result, Result::DROPPED);
    ASSERT_EQ(hold.resume().result, Result::PUSHED);
    EXPECT_EQ(recorder.log.back(), "push j");
    EXPECT_NE(hold.begin_message(), nullptr);
}

TEST(InboundHold, RaisesACloseReportedBeforeTheBindOnceBound) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.report_closed(CloseReason::PEER_CLOSED);
    hold.report_closed(CloseReason::FAILED);
    EXPECT_TRUE(recorder.log.empty());
    hold.bind();
    EXPECT_EQ(recorder.log, std::vector<std::string>({"closed"}));
}

TEST(InboundHold, ADropReleasesTheHeldMessageWithoutAPushAndEndsItsPause) {
    Recorder recorder;
    InboundHold hold = make_hold(recorder);
    hold.bind();
    int token = 0;
    const InboundHold::Outcome offered = receive(hold, "f");
    ASSERT_EQ(offered.result, Result::HELD);
    ASSERT_TRUE(hold.pause(offered.generation, &token));
    hold.report_closed(CloseReason::FAILED);
    EXPECT_TRUE(recorder.log.empty());
    hold.drop();
    const InboundHold::Outcome dropped = hold.resume();
    EXPECT_EQ(dropped.result, Result::DROPPED);
    EXPECT_EQ(dropped.unpause, &token);
    EXPECT_EQ(recorder.log, std::vector<std::string>({"closed"}));
    // Every later message is dropped too
    EXPECT_EQ(receive(hold, "g").result, Result::DROPPED);
}

// The receive task and core race over many messages, the ring refusing at random: every message
// is pushed once, in order, and every pause ends
TEST(InboundHold, AReceiveTaskAndCoreRacingPushEveryMessageOnceInOrder) {
    constexpr int MESSAGES = 2000;
    std::mutex mutex;
    std::vector<std::string> pushed;
    uint32_t seed = 1;
    bool resume_wanted = false;
    InboundHold hold(
        [&](MessageKind /*kind*/, std::span<const uint8_t> payload, int64_t /*rx_us*/) {
            const std::lock_guard<std::mutex> lock(mutex);
            seed = seed * 1103515245U + 12345U;
            if ((seed >> 16U) % 3 == 0) {
                return false;
            }
            pushed.emplace_back(payload.begin(), payload.end());
            return true;
        },
        [&] {
            const std::lock_guard<std::mutex> lock(mutex);
            resume_wanted = true;
        },
        [](CloseReason /*reason*/) {}, KEPT_BYTES);
    hold.bind();
    hold.start();

    std::atomic<bool> done{false};
    std::thread core([&] {
        while (!done.load()) {
            bool resume = false;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                resume = std::exchange(resume_wanted, false);
            }
            if (resume) {
                hold.resume();
            }
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < MESSAGES; ++i) {
        const InboundHold::Outcome offered = receive(hold, std::to_string(i));
        if (offered.result == Result::HELD) {
            int token = 0;
            if (hold.pause(offered.generation, &token)) {
                while (!hold.released(offered.generation)) {
                    std::this_thread::yield();
                }
                EXPECT_EQ(hold.take_pause(), nullptr) << "the release took the pause";
            }
        }
    }
    done = true;
    core.join();
    ASSERT_EQ(pushed.size(), static_cast<size_t>(MESSAGES));
    for (int i = 0; i < MESSAGES; ++i) {
        EXPECT_EQ(pushed[static_cast<size_t>(i)], std::to_string(i));
    }
}

}  // namespace
