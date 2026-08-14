#pragma once

#include "decoder.h"

#include <optional>
#include <span>

namespace pv {

// Owns the complete main-thread/worker transport for the CPU decode stage.
// Pipeline orchestration exchanges value-only commands and completion batches;
// queue synchronization and worker lifetime never escape this boundary.
class DecodeStage {
public:
    DecodeStage(std::size_t capacity, DecodeSlotView slots,
                PngValidationOptions validation);
    ~DecodeStage();

    DecodeStage(const DecodeStage&) = delete;
    DecodeStage& operator=(const DecodeStage&) = delete;

    void Start(std::size_t worker_count);
    void Stop() noexcept;

    [[nodiscard]] HANDLE CompletionEvent() const noexcept;
    [[nodiscard]] const CompletionQueue::Batch& Drain() noexcept;
    [[nodiscard]] bool Submit(DecodeWork work);
    [[nodiscard]] bool Cancel(SlotId staging_slot, DecodeWork& cancelled);
    void Reorder(std::span<const std::size_t> priority);
    void Remap(std::size_t from, std::size_t to, std::uint64_t generation);

    void ResetMetrics() noexcept;
    [[nodiscard]] std::uint64_t DecodeCount() const noexcept;
    [[nodiscard]] std::uint64_t DecodeNanoseconds() const noexcept;
    [[nodiscard]] std::size_t QueuedWorkCount() const;

private:
    WorkQueue work_queue_;
    CompletionQueue completion_queue_;
    CompletionQueue::Batch completion_batch_;
    DecodeSlotView slots_;
    PngValidationOptions validation_;
    std::optional<DecoderPool> workers_;
};

}  // namespace pv
