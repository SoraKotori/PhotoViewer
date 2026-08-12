#include "decode_stage.h"

#include <stdexcept>

namespace pv {

DecodeStage::DecodeStage(const std::size_t capacity, DecodeSlotAccess slots)
    : work_queue_(capacity), completion_queue_(capacity),
      completion_batch_(capacity),
      slots_(slots) {}

DecodeStage::~DecodeStage() { Stop(); }

void DecodeStage::Start(const std::size_t worker_count) {
    if (workers_) throw std::logic_error("decoder workers already started");
    workers_.emplace(worker_count, work_queue_, completion_queue_, slots_);
}

void DecodeStage::Stop() noexcept { workers_.reset(); }

HANDLE DecodeStage::CompletionEvent() const noexcept {
    return completion_queue_.CompletionEvent();
}

const CompletionQueue::Batch& DecodeStage::Drain() noexcept {
    completion_queue_.DrainAll(completion_batch_);
    return completion_batch_;
}

bool DecodeStage::Submit(DecodeWork work) {
    return work_queue_.TryPush(work);
}

bool DecodeStage::Cancel(const SlotId staging_slot, DecodeWork& cancelled) {
    return work_queue_.TryCancel(staging_slot, cancelled);
}

void DecodeStage::Reorder(const std::span<const std::size_t> priority) {
    work_queue_.Reorder(priority);
}

void DecodeStage::Remap(const std::size_t from, const std::size_t to,
                        const std::uint64_t generation) {
    work_queue_.Remap(from, to, generation);
}

void DecodeStage::ResetMetrics() noexcept {
    if (workers_) workers_->ResetMetrics();
}

std::uint64_t DecodeStage::DecodeCount() const noexcept {
    return workers_ ? workers_->DecodeCount() : 0;
}

std::uint64_t DecodeStage::DecodeNanoseconds() const noexcept {
    return workers_ ? workers_->DecodeNanoseconds() : 0;
}

std::size_t DecodeStage::QueuedWorkCount() const { return work_queue_.Size(); }

}  // namespace pv
