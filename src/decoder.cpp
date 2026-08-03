#include "decoder.h"

#include "common.h"
#include "spng_decoder.h"

namespace pv {

DecoderPool::DecoderPool(const std::size_t worker_count, WorkQueue& work_queue,
                         CompletionQueue& completion_queue, const HWND event_window)
    : work_queue_(work_queue), completion_queue_(completion_queue), event_window_(event_window) {
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

void DecoderPool::WorkerMain(const std::stop_token stop) {
    DecodeWork work;
    while (work_queue_.Pop(work, stop)) {
        DecodeResult result = Decode(std::move(work));
        completion_queue_.Push(std::move(result));
        PostMessageW(event_window_, kMessageWorkerComplete, 0, 0);
        work = {};
    }
}

DecodeResult DecoderPool::Decode(DecodeWork work) {
    DecodeResult result;
    result.index = work.index;
    result.generation = work.generation;
    result.surface = std::move(work.surface);

    WorkClaim expected = WorkClaim::Queued;
    if (!work.token || !work.token->claim.compare_exchange_strong(
                           expected, WorkClaim::Claimed, std::memory_order_acq_rel)) {
        result.cancelled = true;
        return result;
    }
    if (!work.compressed || !work.compressed->data || work.compressed->size == 0 ||
        !result.surface ||
        !result.surface->pixels) {
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
        std::span<std::byte>(work.compressed->data, work.compressed->size),
        *result.surface, input_consumed, &callback_context);
    ReleaseInput(work);
    result.error = hr;
    result.cancelled = work.token->claim.load(std::memory_order_acquire) == WorkClaim::Cancelled;
    result.success = SUCCEEDED(hr) && !result.cancelled;
    return result;
}

void DecoderPool::ReleaseInput(DecodeWork& work) noexcept {
    if (!work.compressed) return;
    completion_queue_.PushReleasedInput(ReleasedInput{std::move(work.compressed)});
    PostMessageW(event_window_, kMessageWorkerComplete, 0, 0);
}

}  // namespace pv
