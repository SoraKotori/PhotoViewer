#include "decoder.h"

#include "common.h"
#include "spng_decoder.h"

namespace pv {

DecoderPool::DecoderPool(const std::size_t worker_count, WorkQueue& work_queue,
                         CompletionQueue& completion_queue, ResourceSlots& slots,
                         const HWND event_window)
    : work_queue_(work_queue), completion_queue_(completion_queue), slots_(slots),
      event_window_(event_window) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this](const std::stop_token stop) { WorkerMain(stop); });
    }
}

DecoderPool::~DecoderPool() {
    for (auto& worker : workers_) worker.request_stop();
    work_queue_.Stop();
    workers_.clear();
}

void DecoderPool::ResetMetrics() noexcept {
    decode_count_.store(0, std::memory_order_relaxed);
    decode_nanoseconds_.store(0, std::memory_order_relaxed);
}

std::uint64_t DecoderPool::DecodeCount() const noexcept {
    return decode_count_.load(std::memory_order_relaxed);
}

std::uint64_t DecoderPool::DecodeNanoseconds() const noexcept {
    return decode_nanoseconds_.load(std::memory_order_relaxed);
}

void DecoderPool::WorkerMain(const std::stop_token stop) {
    DecodeWork work;
    while (work_queue_.Pop(work, stop)) {
        const auto begin = std::chrono::steady_clock::now();
        DecodeResult result = Decode(std::move(work));
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin);
        decode_nanoseconds_.fetch_add(static_cast<std::uint64_t>(elapsed.count()),
                                      std::memory_order_relaxed);
        decode_count_.fetch_add(1, std::memory_order_relaxed);
        if (completion_queue_.Push(std::move(result))) {
            PostMessageW(event_window_, kMessageWorkerComplete, 0, 0);
        }
        work = {};
    }
}

DecodeResult DecoderPool::Decode(DecodeWork work) {
    DecodeResult result;
    result.index = work.index;
    result.generation = work.generation;
    result.cpu_surface_slot = work.cpu_surface_slot;
    InputReleaseGuard input_release{*this, work};

    WorkClaim expected = WorkClaim::Queued;
    if (!work.token || !work.token->claim.compare_exchange_strong(
                           expected, WorkClaim::Claimed, std::memory_order_acq_rel)) {
        result.cancelled = true;
        return result;
    }
    if (work.compressed_slot == kInvalidSlot ||
        work.cpu_surface_slot == kInvalidSlot) {
        result.error = E_INVALIDARG;
        return result;
    }
    CompressedBuffer& compressed = slots_.Compressed(work.compressed_slot).resource;
    CpuSurface& surface = slots_.CpuSurfaceAt(work.cpu_surface_slot).resource;
    if (!compressed.data || compressed.size == 0 || !surface.pixels) {
        result.error = E_INVALIDARG;
        return result;
    }
    struct CallbackContext {
        DecoderPool* pool;
        DecodeWork* work;
    } callback_context{this, &work};
    const auto input_consumed = [](void* const raw) noexcept {
        auto* const context = static_cast<CallbackContext*>(raw);
        context->pool->ReleaseInput(*context->work);
    };
    const HRESULT hr = DecodePngSpng(
        std::span<std::byte>(compressed.data, compressed.size),
        surface, input_consumed, &callback_context);
    result.error = hr;
    result.cancelled = work.token->claim.load(std::memory_order_acquire) == WorkClaim::Cancelled;
    result.success = SUCCEEDED(hr) && !result.cancelled;
    return result;
}

void DecoderPool::ReleaseInput(DecodeWork& work) noexcept {
    if (work.compressed_slot == kInvalidSlot) return;
    if (completion_queue_.PushReleasedInput(
            ReleasedInput{work.index, work.generation, work.compressed_slot})) {
        PostMessageW(event_window_, kMessageWorkerComplete, 0, 0);
    }
    work.compressed_slot = kInvalidSlot;
}

}  // namespace pv
