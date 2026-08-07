#pragma once

#include "resource_slots.h"
#include "work_queue.h"

namespace pv {

class DecoderPool {
public:
    DecoderPool(std::size_t worker_count, WorkQueue& work_queue,
                CompletionQueue& completion_queue, ResourceSlots& slots,
                HWND event_window);
    ~DecoderPool();

    void ResetMetrics() noexcept;
    [[nodiscard]] std::uint64_t DecodeCount() const noexcept;
    [[nodiscard]] std::uint64_t DecodeNanoseconds() const noexcept;
    [[nodiscard]] std::size_t SelectedCpuSetCount() const noexcept;
    [[nodiscard]] std::size_t UnthrottledWorkerCount() const noexcept;
    [[nodiscard]] std::size_t ElevatedWorkerCount() const noexcept;

    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

private:
    struct alignas(64) WorkerMetrics {
        std::atomic<std::uint64_t> decode_count{0};
        std::atomic<std::uint64_t> decode_nanoseconds{0};
        std::atomic<bool> selected_cpu_set{false};
        std::atomic<bool> unthrottled{false};
        std::atomic<bool> elevated{false};
        std::array<std::byte,
                   64 - 2 * sizeof(std::atomic<std::uint64_t>) -
                       3 * sizeof(std::atomic<bool>)> cache_line_padding{};
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
    ResourceSlots& slots_;
    HWND event_window_ = nullptr;
    std::vector<std::jthread> workers_;
    std::unique_ptr<WorkerMetrics[]> worker_metrics_;
    std::size_t worker_count_ = 0;
    std::uint64_t decode_count_base_ = 0;
    std::uint64_t decode_nanoseconds_base_ = 0;
};

}  // namespace pv
