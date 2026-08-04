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
    struct InputReleaseGuard {
        DecoderPool& owner;
        DecodeWork& work;
        ~InputReleaseGuard() { owner.ReleaseInput(work); }
    };

    void WorkerMain(std::stop_token stop);
    DecodeResult Decode(DecodeWork work);
    void ReleaseInput(DecodeWork& work) noexcept;

    WorkQueue& work_queue_;
    CompletionQueue& completion_queue_;
    ResourceSlots& slots_;
    HWND event_window_ = nullptr;
    std::vector<std::jthread> workers_;
    std::atomic<std::uint64_t> decode_count_{0};
    std::atomic<std::uint64_t> decode_nanoseconds_{0};
    std::atomic<std::size_t> selected_cpu_set_count_{0};
    std::atomic<std::size_t> unthrottled_worker_count_{0};
    std::atomic<std::size_t> elevated_worker_count_{0};
};

}  // namespace pv
