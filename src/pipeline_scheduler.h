#pragma once

#include "pipeline_types.h"

namespace pv {

class DecodePipeline;
class GraphicsPipeline;
struct PipelineLimits;
class PipelineModel;
class PipelineResources;
class ReservationTable;
class RuntimeTelemetry;
class StoragePipeline;

// Owns main-thread reservation planning and cross-stage dispatch policy.
// Executors retain their own queues and resource lifetimes; this object alone
// decides which frame receives each fixed capacity unit.
class PipelineScheduler {
public:
    PipelineScheduler(const PipelineLimits& limits, PipelineModel& model,
                      PipelineResources& resources, StoragePipeline& storage,
                      DecodePipeline& decode, GraphicsPipeline& graphics,
                      RuntimeTelemetry& telemetry);

    void Pump();
    [[nodiscard]] bool DrainStorage();
    void InitializeReservations();
    void SetCatalogComplete(bool complete) noexcept {
        catalog_complete_ = complete;
    }
    [[nodiscard]] bool CatalogComplete() const noexcept {
        return catalog_complete_;
    }
    [[nodiscard]] PipelineStage StageOf(std::size_t frame) const noexcept;

private:
    void PrepareStorageHeaders();
    void SubmitStorageReads();
    void RebuildReservationPlan();
    void ReconcileReservations();
    [[nodiscard]] bool ReservationActive(const ReservationTable& table,
                                         ReservationId id,
                                         std::size_t frame) const noexcept;

    const PipelineLimits& limits_;
    PipelineModel& model_;
    PipelineResources& resources_;
    StoragePipeline& storage_;
    DecodePipeline& decode_;
    GraphicsPipeline& graphics_;
    RuntimeTelemetry& telemetry_;
    bool catalog_complete_ = true;
};

}  // namespace pv
