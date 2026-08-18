#include "config.h"

#include "win32_support.h"

#include <shellapi.h>

#include <charconv>

namespace pv {
namespace {

std::optional<std::size_t> ParseNonNegative(std::wstring_view value) {
    if (value.empty()) return std::nullopt;
    std::size_t result = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return std::nullopt;
        const std::size_t digit = static_cast<std::size_t>(ch - L'0');
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

std::optional<std::size_t> ParsePositive(const std::wstring_view value) {
    const auto result = ParseNonNegative(value);
    return result && *result != 0 ? result : std::nullopt;
}

std::size_t ParseMibOption(std::wstring_view argument, std::wstring_view prefix) {
    const auto parsed = ParsePositive(argument.substr(prefix.size()));
    if (!parsed || *parsed > std::numeric_limits<std::size_t>::max() / MiB(1)) {
        throw std::invalid_argument("invalid MiB option");
    }
    return MiB(*parsed);
}

PngChunkCrcMode ParseChunkCrcMode(const std::wstring_view value) {
    if (value == L"all") return PngChunkCrcMode::All;
    if (value == L"critical") return PngChunkCrcMode::Critical;
    if (value == L"non-idat") return PngChunkCrcMode::NonIdat;
    if (value == L"none") return PngChunkCrcMode::None;
    throw std::invalid_argument("invalid PNG chunk CRC mode");
}

bool ParseOnOff(const std::wstring_view value, const char* const error) {
    if (value == L"on") return true;
    if (value == L"off") return false;
    throw std::invalid_argument(error);
}

}  // namespace

Config ParseConfig() {
    Config config;

    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) ThrowLastError("CommandLineToArgvW");
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> holder(argv, &LocalFree);
    config.prompt_for_initial_image = count == 1;

    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--validation-exit-after-present") {
            config.validation_exit_after_present = true;
        } else if (argument == L"--validation-elapsed-exit-code") {
            config.validation_elapsed_exit_code = true;
        } else if (argument == L"--validation-fullscreen") {
            config.validation_fullscreen = true;
            config.validation_exit_after_present = true;
        } else if (argument == L"--validation-short-presses") {
            config.validation_short_presses = true;
        } else if (argument.starts_with(L"--validation-navigation=")) {
            config.validation_navigation = argument.substr(
                std::wstring_view(L"--validation-navigation=").size());
            if (config.validation_navigation.empty() ||
                std::any_of(config.validation_navigation.begin(),
                            config.validation_navigation.end(),
                            [](const wchar_t value) { return value != L'L' && value != L'R'; })) {
                throw std::invalid_argument("invalid validation navigation script");
            }
            config.validation_exit_after_present = true;
        } else if (argument.starts_with(L"--validation-file-list=")) {
            config.validation_file_list = argument.substr(
                std::wstring_view(L"--validation-file-list=").size());
            if (config.validation_file_list.empty()) {
                throw std::invalid_argument("invalid validation file list");
            }
        } else if (argument.starts_with(L"--validation-report=")) {
            config.validation_report = argument.substr(
                std::wstring_view(L"--validation-report=").size());
            if (config.validation_report.empty()) {
                throw std::invalid_argument("invalid validation report path");
            }
        } else if (argument.starts_with(L"--staging-cache-mib=")) {
            config.staging_cache_bytes = ParseMibOption(argument, L"--staging-cache-mib=");
        } else if (argument.starts_with(L"--validation-timeout-ms=")) {
            const auto value = ParsePositive(argument.substr(
                std::wstring_view(L"--validation-timeout-ms=").size()));
            if (!value || *value > 600000) {
                throw std::invalid_argument("invalid validation timeout");
            }
            config.validation_timeout_ms = static_cast<std::uint32_t>(*value);
        } else if (argument.starts_with(
                       L"--validation-navigation-start-delay-ms=")) {
            const auto value = ParsePositive(argument.substr(
                std::wstring_view(
                    L"--validation-navigation-start-delay-ms=")
                    .size()));
            if (!value || *value > 60000) {
                throw std::invalid_argument(
                    "invalid validation navigation start delay");
            }
            config.validation_navigation_start_delay_ms =
                static_cast<std::uint32_t>(*value);
        } else if (argument.starts_with(L"--validation-navigation-interval-ms=")) {
            const auto value = ParsePositive(argument.substr(
                std::wstring_view(L"--validation-navigation-interval-ms=").size()));
            if (!value || *value > 1000) {
                throw std::invalid_argument("invalid validation navigation interval");
            }
            config.validation_navigation_interval_ms = static_cast<std::uint32_t>(*value);
        } else if (argument.starts_with(L"--gpu-cache-mib=")) {
            config.gpu_cache_bytes = ParseMibOption(argument, L"--gpu-cache-mib=");
        } else if (argument.starts_with(L"--compressed-budget-mib=")) {
            config.compressed_budget_bytes =
                ParseMibOption(argument, L"--compressed-budget-mib=");
        } else if (argument.starts_with(L"--staging-slot-count=")) {
            const auto value = ParsePositive(argument.substr(
                std::wstring_view(L"--staging-slot-count=").size()));
            if (!value || *value > 4096) throw std::invalid_argument("invalid staging slot count");
            config.staging_slot_count = *value;
        } else if (argument.starts_with(L"--gpu-forward-slot-count=")) {
            const auto value = ParsePositive(argument.substr(
                std::wstring_view(L"--gpu-forward-slot-count=").size()));
            if (!value || *value > 4096) {
                throw std::invalid_argument("invalid GPU forward slot count");
            }
            config.gpu_forward_slot_count = *value;
        } else if (argument.starts_with(L"--gpu-reverse-slot-count=")) {
            const auto value = ParseNonNegative(argument.substr(
                std::wstring_view(L"--gpu-reverse-slot-count=").size()));
            if (!value || *value > 4096) {
                throw std::invalid_argument("invalid GPU reverse slot count");
            }
            config.gpu_reverse_slot_count = *value;
        } else if (argument.starts_with(L"--compressed-slot-count=")) {
            const auto value = ParsePositive(argument.substr(
                std::wstring_view(L"--compressed-slot-count=").size()));
            if (!value || *value > 4096) throw std::invalid_argument("invalid compressed slot count");
            config.compressed_slot_count = *value;
        } else if (argument.starts_with(L"--workers=")) {
            const auto value = ParsePositive(argument.substr(std::wstring_view(L"--workers=").size()));
            if (!value || *value > kMaxDecoderWorkers) {
                throw std::invalid_argument("invalid worker count");
            }
            config.worker_count = *value;
        } else if (argument.starts_with(L"--png-chunk-crc=")) {
            config.png_validation.chunk_crc = ParseChunkCrcMode(
                argument.substr(
                    std::wstring_view(L"--png-chunk-crc=").size()));
        } else if (argument.starts_with(L"--png-adler32=")) {
            config.png_validation.adler32 = ParseOnOff(
                argument.substr(std::wstring_view(L"--png-adler32=").size()),
                "invalid PNG Adler-32 mode");
        } else if (argument.starts_with(L"--")) {
            throw std::invalid_argument("unknown option");
        } else if (config.initial_image.empty()) {
            config.initial_image = argument;
        } else {
            throw std::invalid_argument("multiple image paths supplied");
        }
    }
    if (config.GpuSlotCount() > 4096) {
        throw std::invalid_argument("combined GPU slot count exceeds 4096");
    }
    return config;
}

}  // namespace pv
