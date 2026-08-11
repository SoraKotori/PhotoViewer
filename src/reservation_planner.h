#pragma once

#include "config.h"
#include "navigation.h"
#include "pipeline_types.h"
#include "reservation.h"
#include "work_queue.h"

namespace pv {

class ReservationPlanner {
public:
    explicit ReservationPlanner(const Config& config);

    void Reset(std::size_t compressed_capacity,
               std::size_t staging_capacity,
               std::size_t gpu_texture_capacity);
    void MarkDirty() noexcept { dirty_ = true; }
    [[nodiscard]] bool NeedsRebuild(
        const std::vector<ImageRecord>& images) const noexcept;
    void Rebuild(const NavigationState& navigation,
                 const std::vector<ImageRecord>& images,
                 WorkQueue& work_queue);

    [[nodiscard]] ReservationTable& Compressed() noexcept {
        return compressed_;
    }
    [[nodiscard]] const ReservationTable& Compressed() const noexcept {
        return compressed_;
    }
    [[nodiscard]] ReservationTable& Staging() noexcept { return staging_; }
    [[nodiscard]] const ReservationTable& Staging() const noexcept {
        return staging_;
    }
    [[nodiscard]] ReservationTable& GpuTextures() noexcept {
        return gpu_textures_;
    }
    [[nodiscard]] const ReservationTable& GpuTextures() const noexcept {
        return gpu_textures_;
    }
    [[nodiscard]] const std::vector<std::size_t>& PriorityOrder() const noexcept {
        return priority_order_;
    }
    [[nodiscard]] const std::vector<std::size_t>& DesiredGpuTextures() const noexcept {
        return desired_gpu_textures_;
    }
    [[nodiscard]] std::vector<std::size_t>& PrepareStagingDesired() noexcept;
    [[nodiscard]] std::vector<std::size_t>& PrepareCompressedDesired() noexcept;

private:
    static void AppendUnique(std::vector<std::size_t>& frames,
                             std::size_t frame,
                             std::size_t capacity);

    std::size_t gpu_forward_capacity_ = 0;
    std::size_t gpu_reverse_capacity_ = 0;
    ReservationTable compressed_;
    ReservationTable staging_;
    ReservationTable gpu_textures_;
    std::vector<std::size_t> priority_order_;
    std::vector<std::size_t> planning_scratch_;
    std::vector<std::size_t> desired_gpu_textures_;
    std::vector<std::size_t> desired_staging_;
    std::vector<std::size_t> desired_compressed_;
    bool dirty_ = true;
};

}  // namespace pv
