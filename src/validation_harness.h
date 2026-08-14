#pragma once

#include "config.h"
#include "runtime_telemetry.h"
#include "validation.h"

#include <optional>

namespace pv {

class PipelineRuntime;
class ViewerWindow;

class ValidationHarness {
public:
    ValidationHarness(const Config& config,
                      std::chrono::steady_clock::time_point process_started)
        : config_(config),
          telemetry_(config.validation_navigation.empty()
                         ? std::chrono::steady_clock::time_point{}
                         : process_started),
          session_(!config.validation_navigation.empty(),
                   config.validation_navigation.size() + 1) {}

    [[nodiscard]] RuntimeTelemetry& Telemetry() noexcept { return telemetry_; }
    void MarkWindowReady() noexcept;
    void MarkCatalogReady() noexcept;
    [[nodiscard]] bool NavigationTimerActive() const noexcept;

    void BeginFullscreenValidation(ViewerWindow& window);
    [[nodiscard]] std::optional<int> OnFullscreenValidationTimer(
        ViewerWindow& window, PipelineRuntime& pipeline);
    void InjectValidationNavigation(PipelineRuntime& pipeline,
                                    ViewerWindow& window);
    void InjectValidationNavigationStep(PipelineRuntime& pipeline,
                                        ViewerWindow& window);
    void StopValidationNavigationTimer(ViewerWindow& window);
    void OnFrameReady(std::size_t index);
    [[nodiscard]] std::optional<int> OnFramePresented(
        std::size_t index, PipelineRuntime& pipeline, ViewerWindow& window);
    [[nodiscard]] int OnTimeout(PipelineRuntime& pipeline);

private:
    void RecordPresentation(std::size_t index);
    void WriteReport(PipelineRuntime& pipeline, std::string_view phase,
                     bool truncate);

    const Config& config_;
    RuntimeTelemetry telemetry_;
    ValidationSession session_;
};

}  // namespace pv
