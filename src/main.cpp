#include "app.h"
#include "common.h"
#include "config.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    try {
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
            GetLastError() != ERROR_ACCESS_DENIED) {
            pv::ThrowLastError("SetProcessDpiAwarenessContext");
        }
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        pv::CheckHr(initialized, "CoInitializeEx");
        const int result = pv::App(pv::ParseConfig()).Run(instance, show_command);
        CoUninitialize();
        return result;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "PhotoViewer", MB_OK | MB_ICONERROR);
        return 1;
    }
}
