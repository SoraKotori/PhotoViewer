#include "decoder.h"

#include "common.h"
#include "spng_decoder.h"

namespace pv {
namespace {

struct CpuSetCandidate {
    DWORD id = 0;
    WORD group = 0;
    BYTE core = 0;
    BYTE logical = 0;
    BYTE efficiency = 0;
    BYTE scheduling = 0;
    bool parked = false;
};

std::size_t ValidateWorkerCount(const std::size_t worker_count) {
    if (worker_count == 0 || worker_count > kMaxDecoderWorkers) {
        throw std::invalid_argument("invalid decoder worker count");
    }
    return worker_count;
}

std::vector<DWORD> SelectCpuSets(const std::size_t worker_count) {
    ULONG required = 0;
    GetSystemCpuSetInformation(nullptr, 0, &required, GetCurrentProcess(), 0);
    if (required == 0) return {};
    std::vector<std::byte> buffer(required);
    if (!GetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
            required, &required, GetCurrentProcess(), 0)) {
        return {};
    }

    std::vector<CpuSetCandidate> candidates;
    for (std::size_t offset = 0; offset < required;) {
        const auto* const information =
            reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (information->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) ||
            information->Size > required - offset) {
            return {};
        }
        if (information->Type == CpuSetInformation) {
            const auto& cpu = information->CpuSet;
            if (!cpu.Allocated || cpu.AllocatedToTargetProcess) {
                candidates.push_back(CpuSetCandidate{
                    cpu.Id, cpu.Group, cpu.CoreIndex, cpu.LogicalProcessorIndex,
                    cpu.EfficiencyClass, cpu.SchedulingClass, cpu.Parked != 0});
            }
        }
        offset += information->Size;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CpuSetCandidate& left, const CpuSetCandidate& right) {
        return std::tuple{left.parked, static_cast<int>(-left.efficiency),
                          static_cast<int>(-left.scheduling), left.group,
                          left.core, left.logical} <
               std::tuple{right.parked, static_cast<int>(-right.efficiency),
                          static_cast<int>(-right.scheduling), right.group,
                          right.core, right.logical};
    });

    std::vector<DWORD> selected;
    selected.reserve(candidates.size());
    std::vector<std::pair<WORD, BYTE>> selected_cores;
    if (!candidates.empty()) {
        const BYTE best_efficiency = candidates.front().efficiency;
        const BYTE best_scheduling = candidates.front().scheduling;
        for (const CpuSetCandidate& candidate : candidates) {
            if (candidate.parked || candidate.efficiency != best_efficiency ||
                candidate.scheduling != best_scheduling) {
                continue;
            }
            const auto core = std::pair{candidate.group, candidate.core};
            if (std::find(selected_cores.begin(), selected_cores.end(), core) !=
                selected_cores.end()) {
                continue;
            }
            selected.push_back(candidate.id);
            selected_cores.push_back(core);
        }
    }
    if (selected.size() >= worker_count) return selected;
    for (const CpuSetCandidate& candidate : candidates) {
        const auto core = std::pair{candidate.group, candidate.core};
        if (std::find(selected_cores.begin(), selected_cores.end(), core) !=
            selected_cores.end()) {
            continue;
        }
        selected.push_back(candidate.id);
        selected_cores.push_back(core);
        if (selected.size() >= worker_count) return selected;
    }
    for (const CpuSetCandidate& candidate : candidates) {
        if (std::find(selected.begin(), selected.end(), candidate.id) != selected.end()) {
            continue;
        }
        selected.push_back(candidate.id);
        if (selected.size() >= worker_count) break;
    }
    return selected;
}

}  // namespace

DecoderPool::DecoderPool(const std::size_t worker_count, WorkQueue& work_queue,
                         CompletionQueue& completion_queue, ResourceSlots& slots)
    : work_queue_(work_queue), completion_queue_(completion_queue), slots_(slots),
      worker_metrics_(ValidateWorkerCount(worker_count)) {
    const std::vector<DWORD> cpu_sets = SelectCpuSets(worker_count);
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        const bool has_cpu_set = index < cpu_sets.size();
        const DWORD cpu_set = has_cpu_set ? cpu_sets[index] : 0;
        WorkerMetrics* const metrics = &worker_metrics_[index];
        workers_.emplace_back([this, has_cpu_set, cpu_set, metrics](
                                  const std::stop_token stop) {
            if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
                metrics->elevated.store(true, std::memory_order_relaxed);
            }
            THREAD_POWER_THROTTLING_STATE power{};
            power.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
            power.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
            if (SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling,
                                     &power, sizeof(power))) {
                metrics->unthrottled.store(true, std::memory_order_relaxed);
            }
            if (has_cpu_set &&
                SetThreadSelectedCpuSets(GetCurrentThread(), &cpu_set, 1)) {
                metrics->selected_cpu_set.store(true, std::memory_order_relaxed);
            }
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

std::size_t DecoderPool::SelectedCpuSetCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < worker_metrics_.size(); ++index) {
        count += worker_metrics_[index].selected_cpu_set.load(
            std::memory_order_relaxed);
    }
    return count;
}

std::size_t DecoderPool::UnthrottledWorkerCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < worker_metrics_.size(); ++index) {
        count += worker_metrics_[index].unthrottled.load(
            std::memory_order_relaxed);
    }
    return count;
}

std::size_t DecoderPool::ElevatedWorkerCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < worker_metrics_.size(); ++index) {
        count += worker_metrics_[index].elevated.load(std::memory_order_relaxed);
    }
    return count;
}

void DecoderPool::WorkerMain(const std::stop_token stop, WorkerMetrics& metrics) {
    DecodeWork work;
    std::uint64_t decode_count = 0;
    std::uint64_t decode_nanoseconds = 0;
    while (work_queue_.Pop(work, stop)) {
        const auto begin = std::chrono::steady_clock::now();
        DecodeResult result = Decode(std::move(work));
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin);
        decode_nanoseconds += static_cast<std::uint64_t>(elapsed.count());
        ++decode_count;
        metrics.decode_nanoseconds.store(decode_nanoseconds,
                                         std::memory_order_relaxed);
        metrics.decode_count.store(decode_count, std::memory_order_relaxed);
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
    CompressedBuffer& compressed = slots_.Compressed(work.compressed_slot).resource;
    DecodeSurface& surface = slots_.StagingAt(work.staging_slot).resource.surface;
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
