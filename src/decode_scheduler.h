#pragma once

#include "decoded_image.h"
#include "telemetry.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

constexpr UINT kDecodeReadyMessage = WM_APP + 1;
constexpr UINT kDecodeFailedMessage = WM_APP + 2;

struct DecodeInventorySnapshot
{
    std::size_t readyAhead{};
    std::size_t cachedImages{};
    std::size_t inFlightImages{};
    std::size_t desiredImages{};
    std::size_t cacheBytes{};
};

class DecodeScheduler final
{
public:
    explicit DecodeScheduler(Telemetry& telemetry);
    ~DecodeScheduler();

    DecodeScheduler(const DecodeScheduler&) = delete;
    DecodeScheduler& operator=(const DecodeScheduler&) = delete;

    void start(
        HWND notificationWindow,
        std::vector<std::filesystem::path> catalog,
        std::size_t initialIndex,
        std::size_t workerCount = 18);
    void stop();
    void updateDemand(
        std::size_t displayedIndex,
        std::optional<std::size_t> foregroundIndex,
        int direction,
        bool includeWorkerPipeline);
    [[nodiscard]] std::shared_ptr<const DecodedImage> tryGet(std::size_t index);
    [[nodiscard]] DecodeInventorySnapshot inventory(std::size_t displayedIndex, int direction) const;
    [[nodiscard]] std::size_t catalogSize() const noexcept;
    [[nodiscard]] const std::filesystem::path& pathAt(std::size_t index) const;

private:
    struct Task
    {
        std::size_t index{};
        std::uint32_t priority{};
        std::uint64_t version{};
        std::uint64_t sequence{};
    };

    struct TaskCompare
    {
        [[nodiscard]] bool operator()(const Task& left, const Task& right) const noexcept;
    };

    struct CacheEntry
    {
        std::shared_ptr<DecodedImage> image;
        std::uint64_t lastUse{};
    };

    [[nodiscard]] bool taskIsCurrentLocked(const Task& task) const;
    [[nodiscard]] std::optional<Task> takeForegroundTaskLocked();
    [[nodiscard]] std::optional<Task> takeBackgroundTaskLocked();
    void workerLoop(std::size_t workerSlot);
    void finishDecode(const Task& task, std::shared_ptr<DecodedImage> image, HRESULT result);
    void trimCacheLocked();

    Telemetry& telemetry_;
    HWND notificationWindow_{};
    std::vector<std::filesystem::path> catalog_;
    std::vector<std::size_t> expectedDecodedBytes_;
    std::vector<std::thread> workers_;
    std::priority_queue<Task, std::vector<Task>, TaskCompare> tasks_;
    std::unordered_map<std::size_t, std::uint32_t> desiredPriorities_;
    std::vector<std::uint64_t> taskVersions_;
    std::unordered_set<std::size_t> inFlight_;
    std::unordered_set<std::size_t> failed_;
    std::unordered_map<std::size_t, CacheEntry> cache_;
    std::optional<std::size_t> foregroundIndex_;
    std::size_t displayedIndex_{};
    std::size_t cacheBytes_{};
    std::size_t cacheBudgetBytes_{2304ULL * 1024ULL * 1024ULL};
    std::size_t workerCount_{1};
    std::uint64_t workerStaggerMicroseconds_{};
    std::uint64_t taskSequence_{};
    std::uint64_t useClock_{};
    bool stopping_{true};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};
