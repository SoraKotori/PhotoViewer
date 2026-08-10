#include "app.h"
#include "common.h"
#include "config.h"

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ PWSTR, _In_ int show_command) {
    const auto process_started = std::chrono::steady_clock::now();
    try {
        return pv::App(pv::ParseConfig(), process_started)
            .Run(instance, show_command);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "PhotoViewer", MB_OK | MB_ICONERROR);
        return 1;
    }
}
