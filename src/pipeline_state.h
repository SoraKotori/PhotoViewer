#pragma once

#include "pipeline_types.h"

namespace pv {

class PipelineResources;
class ReservationPlanner;
class ReservationTable;

[[nodiscard]] bool IsReservationActive(const ReservationTable& table,
                                       ReservationId id,
                                       std::size_t frame) noexcept;

// IOCP intentionally omits packets for synchronous ReadFile success. A pump
// pass must therefore continue when inline storage completion changed state,
// even if no frame was presented in that pass.
[[nodiscard]] constexpr bool ShouldContinuePipelinePass(
    const bool synchronous_storage_progress,
    const bool presented) noexcept {
    return synchronous_storage_progress || presented;
}

[[nodiscard]] PipelineStage DeterminePipelineStage(
    std::size_t frame, const ImageRecord& image,
    const PipelineResources& resources,
    const ReservationPlanner& reservations) noexcept;

}  // namespace pv
