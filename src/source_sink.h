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

/// @file source_sink.h
/// @brief The seam between a routed source's decoder and the group it plays in: SourcePcmSink
/// takes the decoded PCM with each block's capture time. LiveSourceSink writes it into an
/// ordinary live input of the group, on the timeline the server chooses, and uses the capture
/// times only to drop a stall's backlog and to start a new run after a long gap
/// (judge_source_chunk()).

#pragma once

#include "host_view.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace sendspin::server {

/// @brief A gap in a source's capture times this long or longer ends the input's run, so its next
/// audio starts a new run with a fresh lead rather than playing late behind one long gap
constexpr int64_t SOURCE_RESTART_GAP_US = 1000000;

/// @brief The decoded audio of one routed source's input streams, in order. Called on the
/// source's group audio job, one call at a time.
class SourcePcmSink {
public:
    virtual ~SourcePcmSink() = default;

    /// @brief A stream opened, or the open one took a new format in place; write() takes this
    /// format after it
    /// @return false when the group's input cannot take it
    virtual bool begin(const InputFormat& format) = 0;

    /// @brief One chunk's whole audio, however many codec frames it decoded from, so the chunk is
    /// judged as one
    /// @param pcm frames frames in begin()'s format, at least one
    /// @param capture_us the server-clock time the first frame was captured (roles/source/v1.md
    /// "Source Audio Chunks (Binary)"), within MAX_SOURCE_TIMESTAMP_US of 0
    /// @param rx_us the receive stamp of the chunk
    virtual void write(const uint8_t* pcm, size_t frames, int64_t capture_us, int64_t rx_us) = 0;

    /// @brief The stream ended
    virtual void end() = 0;
};

/// @brief What judge_source_chunk() keeps between a stream's chunks
struct SourceTiming {
    /// The least lateness (receive stamp less capture end) seen since the stream began or was
    /// rebased: the source's path when nothing holds its audio back
    std::optional<int64_t> baseline_us;
    /// The capture end of the last block written
    std::optional<int64_t> written_end_us;
    /// The audio dropped in a row as stale, in microseconds
    int64_t stale_us{0};
};

enum class SourceChunkAction : uint8_t {
    WRITE,
    /// Captured past SOURCE_RESTART_GAP_US after the last block written: end the run, then write
    RESTART,
    /// Later than the baseline by more than the input's write jitter: a stall's backlog, which the
    /// spec asks sources to drop (roles/source/v1.md "Source Audio Chunks (Binary)"); dropped so
    /// it does not raise the live input's latency
    DROP,
};

/// @brief What to do with a block captured from capture_us to end_us and received at rx_us. A
/// stale run longer than SOURCE_RESTART_GAP_US is a jump in the source's time mapping rather than
/// a backlog (the spec's "Source timestamps ... may show discontinuities"): the baseline restarts
/// there. Capture times that overlap or step back are written; they are not cut points.
SourceChunkAction judge_source_chunk(SourceTiming& timing, int64_t capture_us, int64_t end_us,
                                     int64_t rx_us, int64_t jitter_us);

/// @brief Writes a source's audio into a live input (InputKind::LIVE). The input's own rules apply:
/// a write it refuses is dropped, a late write moves its timeline forward in one gap, and a stop by
/// the server holds until the host acknowledges it.
class LiveSourceSink : public SourcePcmSink {
public:
    /// @param jitter_us the input's write jitter, the staleness bound judge_source_chunk() uses
    LiveSourceSink(std::shared_ptr<HostView> view, int64_t jitter_us);

    bool begin(const InputFormat& format) override;
    void write(const uint8_t* pcm, size_t frames, int64_t capture_us, int64_t rx_us) override;
    /// @brief Ends the input's run, so its players' streams end with the source's
    void end() override;

private:
    /// The run ends unless a stop by the server holds it or the input closed
    void stop_run();

    std::shared_ptr<HostView> view_;
    int64_t jitter_us_;
    uint32_t sample_rate_{0};
    SourceTiming timing_;
};

}  // namespace sendspin::server
