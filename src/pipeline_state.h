#pragma once

#include "pipeline_types.h"

namespace pv {

class PipelineResources;
class ReservationPlanner;
class ReservationTable;

[[nodiscard]] bool IsReservationActive(const ReservationTable& table,
                                       ReservationId id,
                                       std::size_t frame) noexcept;

[[nodiscard]] PipelineStage DeterminePipelineStage(
    std::size_t frame, const ImageRecord& image,
    const PipelineResources& resources,
    const ReservationPlanner& reservations) noexcept;

}  // namespace pv
