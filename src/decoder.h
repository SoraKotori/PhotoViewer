#pragma once

#include "png_validation.h"
#include "resource_slots.h"
#include "work_queue.h"

#include <array>

namespace pv {

class DecoderPool {
public:
    DecoderPool(std::size_t worker_count, WorkQueue& work_queue,
                CompletionQueue& completion_queue, DecodeSlotAccess& slots,
                PngValidationOptions validation);
    ~DecoderPool();

    void ResetMetrics() noexcept;
    [[nodiscard]] std::uint64_t DecodeCount() const noexcept;
    [[nodiscard]] std::uint64_t DecodeNanoseconds() const noexcept;

    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

private:
    struct alignas(64) WorkerMetrics {
        std::atomic<std::uint64_t> decode_count{0};
        std::atomic<std::uint64_t> decode_nanoseconds{0};
        std::array<std::byte,
                   64 - 2 * sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
    };
    static_assert(sizeof(WorkerMetrics) == 64);

    struct InputReleaseGuard {
        DecoderPool& owner;
        DecodeWork& work;
        ~InputReleaseGuard() { owner.ReleaseInput(work); }
    };

    void WorkerMain(std::stop_token stop, WorkerMetrics& metrics);
    DecodeResult Decode(DecodeWork work);
    void ReleaseInput(DecodeWork& work) noexcept;

    WorkQueue& work_queue_;
    CompletionQueue& completion_queue_;
    DecodeSlotAccess& slots_;
    const PngValidationOptions validation_;
    // Constructed once at the configured size and never resized. This keeps
    // the runtime-sized, cache-line-aligned sequence off the main stack.
    std::vector<WorkerMetrics> worker_metrics_;
    // Declared after worker_metrics_ so partial-construction unwinding joins
    // every worker before releasing the metrics captured by those threads.
    std::vector<std::jthread> workers_;
    std::atomic<bool> metrics_enabled_{false};
    std::uint64_t decode_count_base_ = 0;
    std::uint64_t decode_nanoseconds_base_ = 0;
};

}  // namespace pv
