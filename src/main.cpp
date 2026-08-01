#include "app.h"
#include "telemetry.h"

#include <filesystem>
#include <optional>
#include <shellapi.h>
#include <string>
#include <windows.h>

namespace
{
[[nodiscard]] std::optional<AppOptions> parseOptions()
{
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return std::nullopt;
    }

    AppOptions options{};
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument = arguments[index];
        if (argument == L"--telemetry-pipe" && index + 1 < argumentCount) {
            options.telemetryPipe = arguments[++index];
        } else if (argument == L"--catalog-manifest" && index + 1 < argumentCount) {
            options.catalogManifest = arguments[++index];
        } else if (argument == L"--decode-workers" && index + 1 < argumentCount) {
            try {
                options.decodeWorkers = static_cast<std::size_t>(std::stoull(arguments[++index]));
            } catch (const std::exception&) {
                LocalFree(arguments);
                return std::nullopt;
            }
            if (options.decodeWorkers == 0 || options.decodeWorkers > 32) {
                LocalFree(arguments);
                return std::nullopt;
            }
        } else if (argument == L"--no-persistent-state") {
            options.noPersistentState = true;
        } else if (!argument.starts_with(L"--") && options.initialImage.empty()) {
            options.initialImage = std::filesystem::path(argument);
        } else {
            LocalFree(arguments);
            return std::nullopt;
        }
    }
    LocalFree(arguments);
    if (options.initialImage.empty()) {
        return std::nullopt;
    }
    return options;
}

void showError(const wchar_t* message)
{
    static_cast<void>(MessageBoxW(nullptr, message, L"PhotoViewer", MB_OK | MB_ICONERROR));
}
} // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, const int showCommand)
{
    const std::optional<AppOptions> options = parseOptions();
    if (!options) {
        showError(L"Usage: PhotoViewer.exe <image.png> [--decode-workers <1-32>] [--telemetry-pipe <name>] [--no-persistent-state]");
        return 2;
    }

    Telemetry telemetry;
    if (!telemetry.connect(options->telemetryPipe)) {
        showError(L"Unable to connect to the acceptance telemetry pipe.");
        return 3;
    }

    App app(telemetry);
    if (!app.initialize(instance, showCommand, *options)) {
        showError(L"Unable to initialize PhotoViewer or open the selected PNG folder.");
        return 1;
    }
    return app.run();
}
