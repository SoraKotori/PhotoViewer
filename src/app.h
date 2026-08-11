#pragma once

#include "catalog.h"
#include "config.h"
#include "pipeline_runtime.h"
#include "validation.h"
#include "viewer_window.h"

namespace pv {

class App : private PipelineObserver {
public:
    App(Config config, std::chrono::steady_clock::time_point process_started);
    ~App();

    int Run(HINSTANCE instance, int show_command);

private:
    friend class PipelineRuntime;
    struct ComApartment {
        ComApartment();
        ~ComApartment();

        ComApartment(const ComApartment&) = delete;
        ComApartment& operator=(const ComApartment&) = delete;
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam);
    int EventLoop();
    LRESULT HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void OpenInitialImage();
    void OnDirection(int direction, bool repeat, std::size_t repeat_count);
    void OnDirectionReleased(int direction);
    void OnCatalogComplete();
    void OnFrameReady(std::size_t index) override;
    void OnFramePresented(std::size_t index) override;
    void ToggleFullscreen();
    void InjectValidationNavigation();
    void InjectValidationNavigationStep();
    void StopValidationNavigationTimer();
    void RecordValidationReady(std::size_t index);
    void RecordValidationPresentation(std::size_t index);
    void WriteValidationReport(std::string_view phase, bool truncate);
    void BeginFullscreenValidation();
    void OnFullscreenValidationTimer();

    Config config_;
    ValidationState validation_;
    ViewerWindow window_;
    bool running_ = true;
    bool catalog_loading_ = false;
    int exit_code_ = 0;

    std::optional<ComApartment> com_apartment_;
    std::optional<AsyncCatalog> catalog_io_;
    PipelineRuntime pipeline_;
};

}  // namespace pv
