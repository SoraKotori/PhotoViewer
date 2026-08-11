#include "reservation_planner.h"

namespace pv {

ReservationPlanner::ReservationPlanner(const Config& config)
    : gpu_forward_capacity_(config.gpu_forward_slot_count),
      gpu_reverse_capacity_(config.gpu_reverse_slot_count) {
    const std::size_t planning_capacity = config.compressed_slot_count +
        config.staging_slot_count + config.GpuSlotCount();
    priority_order_.reserve(planning_capacity);
    planning_scratch_.reserve(planning_capacity);
    desired_gpu_textures_.reserve(config.GpuSlotCount());
    desired_staging_.reserve(config.staging_slot_count);
    desired_compressed_.reserve(config.compressed_slot_count);
}

void ReservationPlanner::Reset(const std::size_t compressed_capacity,
                               const std::size_t staging_capacity,
                               const std::size_t gpu_texture_capacity) {
    compressed_.Reset(compressed_capacity);
    staging_.Reset(staging_capacity);
    gpu_textures_.Reset(gpu_texture_capacity);
    priority_order_.clear();
    planning_scratch_.clear();
    desired_gpu_textures_.clear();
    desired_staging_.clear();
    desired_compressed_.clear();
    dirty_ = true;
}

bool ReservationPlanner::NeedsRebuild(
    const std::vector<ImageRecord>& images) const noexcept {
    if (dirty_) return true;
    return std::any_of(
        desired_gpu_textures_.begin(), desired_gpu_textures_.end(),
        [&](const std::size_t frame) {
            return frame >= images.size() || images[frame].failed;
        });
}

void ReservationPlanner::AppendUnique(std::vector<std::size_t>& frames,
                                      const std::size_t frame,
                                      const std::size_t capacity) {
    if (frames.size() < capacity &&
        std::find(frames.begin(), frames.end(), frame) == frames.end()) {
        frames.push_back(frame);
    }
}

void ReservationPlanner::Rebuild(const NavigationState& navigation,
                                 const std::vector<ImageRecord>& images,
                                 WorkQueue& work_queue) {
    const std::size_t frame_count = images.size();
    const auto add_capacity = [frame_count](const std::size_t left,
                                             const std::size_t right) {
        return left >= frame_count - std::min(right, frame_count)
                   ? frame_count
                   : left + right;
    };
    std::size_t planning_capacity = compressed_.Capacity();
    planning_capacity = add_capacity(planning_capacity, staging_.Capacity());
    planning_capacity = add_capacity(planning_capacity, gpu_textures_.Capacity());
    navigation.BuildPlan(planning_capacity, priority_order_);

    desired_gpu_textures_.clear();
    const std::size_t gpu_capacity = gpu_textures_.Capacity();
    const std::size_t forward_capacity = std::min(
        gpu_forward_capacity_, gpu_capacity);
    const std::size_t current = navigation.CurrentIndex();
    AppendUnique(desired_gpu_textures_, current, gpu_capacity);
    for (const std::size_t frame : priority_order_) {
        if (desired_gpu_textures_.size() >= forward_capacity) break;
        if (!images[frame].failed) {
            AppendUnique(desired_gpu_textures_, frame, gpu_capacity);
        }
    }

    const std::size_t reverse_capacity = std::min(
        gpu_reverse_capacity_, gpu_capacity - desired_gpu_textures_.size());
    int direction = navigation.PreferredDirection();
    if (direction == 0) direction = 1;
    for (std::size_t distance = 1; distance <= reverse_capacity; ++distance) {
        if (direction > 0) {
            if (distance > current) break;
            AppendUnique(desired_gpu_textures_, current - distance, gpu_capacity);
        } else {
            if (distance >= images.size() - current) break;
            AppendUnique(desired_gpu_textures_, current + distance, gpu_capacity);
        }
    }
    for (const std::size_t frame : priority_order_) {
        if (!images[frame].failed) {
            AppendUnique(desired_gpu_textures_, frame, gpu_capacity);
        }
    }

    planning_scratch_.clear();
    for (const std::size_t frame : desired_gpu_textures_) {
        AppendUnique(planning_scratch_, frame, images.size());
    }
    for (const std::size_t frame : priority_order_) {
        AppendUnique(planning_scratch_, frame, images.size());
    }
    priority_order_.swap(planning_scratch_);
    work_queue.Reorder(priority_order_);
    dirty_ = false;
}

std::vector<std::size_t>& ReservationPlanner::PrepareStagingDesired() noexcept {
    desired_staging_.clear();
    return desired_staging_;
}

std::vector<std::size_t>& ReservationPlanner::PrepareCompressedDesired() noexcept {
    desired_compressed_.clear();
    return desired_compressed_;
}

}  // namespace pv
