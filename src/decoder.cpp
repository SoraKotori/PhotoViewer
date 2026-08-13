#include "decoder.h"

#include "win32_support.h"
#include "spng_decoder.h"

namespace pv {
namespace {

std::size_t ValidateWorkerCount(const std::size_t worker_count) {
    if (worker_count == 0 || worker_count > kMaxDecoderWorkers) {
        throw std::invalid_argument("invalid decoder worker count");
    }
    return worker_count;
}

}  // namespace

DecoderPool::DecoderPool(const std::size_t worker_count, WorkQueue& work_queue,
                         CompletionQueue& completion_queue,
                         DecodeSlotView& slots,
                         const PngValidationOptions validation)
    : work_queue_(work_queue), completion_queue_(completion_queue), slots_(slots),
      validation_(validation),
      worker_metrics_(ValidateWorkerCount(worker_count)) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        WorkerMetrics* const metrics = &worker_metrics_[index];
        workers_.emplace_back([this, metrics](const std::stop_token stop) {
            WorkerMain(stop, *metrics);
        });
    }
}

DecoderPool::~DecoderPool() {
    for (auto& worker : workers_) worker.request_stop();
    work_queue_.Stop();
    workers_.clear();
}

void DecoderPool::ResetMetrics() noexcept {
    std::uint64_t count = 0;
    std::uint64_t nanoseconds = 0;
    for (std::size_t index = 0; index < worker_metrics_.size(); ++index) {
        count += worker_metrics_[index].decode_count.load(
            std::memory_order_relaxed);
        nanoseconds += worker_metrics_[index].decode_nanoseconds.load(
            std::memory_order_relaxed);
    }
    decode_count_base_ = count;
    decode_nanoseconds_base_ = nanoseconds;
    metrics_enabled_.store(true, std::memory_order_relaxed);
}

std::uint64_t DecoderPool::DecodeCount() const noexcept {
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < worker_metrics_.size(); ++index) {
        total += worker_metrics_[index].decode_count.load(std::memory_order_relaxed);
    }
    return total >= decode_count_base_ ? total - decode_count_base_ : 0;
}

std::uint64_t DecoderPool::DecodeNanoseconds() const noexcept {
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < worker_metrics_.size(); ++index) {
        total += worker_metrics_[index].decode_nanoseconds.load(
            std::memory_order_relaxed);
    }
    return total >= decode_nanoseconds_base_
               ? total - decode_nanoseconds_base_
               : 0;
}

void DecoderPool::WorkerMain(const std::stop_token stop, WorkerMetrics& metrics) {
    DecodeWork work;
    std::uint64_t decode_count = 0;
    std::uint64_t decode_nanoseconds = 0;
    while (work_queue_.Pop(work, stop)) {
        const bool measure = metrics_enabled_.load(std::memory_order_relaxed);
        const auto begin = measure ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        DecodeResult result = Decode(std::move(work));
        if (measure) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin);
            decode_nanoseconds += static_cast<std::uint64_t>(elapsed.count());
            ++decode_count;
            metrics.decode_nanoseconds.store(decode_nanoseconds,
                                             std::memory_order_relaxed);
            metrics.decode_count.store(decode_count, std::memory_order_relaxed);
        }
        completion_queue_.Push(std::move(result));
        work = {};
    }
}

DecodeResult DecoderPool::Decode(DecodeWork work) {
    DecodeResult result;
    result.index = work.index;
    result.generation = work.generation;
    result.staging_slot = work.staging_slot;
    InputReleaseGuard input_release{*this, work};

    if (work.compressed_slot == kInvalidSlot ||
        work.staging_slot == kInvalidSlot) {
        result.error = E_INVALIDARG;
        return result;
    }
    const std::span<std::byte> compressed =
        slots_.CompressedInput(work.compressed_slot);
    DecodeSurface& surface = slots_.DecodeOutput(work.staging_slot);
    const PngResourcePlan& expected = slots_.ExpectedPng(work.staging_slot);
    if (compressed.empty() || !surface.pixels) {
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
        compressed, surface, expected, validation_, input_consumed,
        &callback_context);
    result.error = hr;
    result.success = SUCCEEDED(hr);
    return result;
}

void DecoderPool::ReleaseInput(DecodeWork& work) noexcept {
    if (work.compressed_slot == kInvalidSlot) return;
    completion_queue_.PushReleasedInput(
        ReleasedInput{work.index, work.generation, work.compressed_slot});
    work.compressed_slot = kInvalidSlot;
}

}  // namespace pv
