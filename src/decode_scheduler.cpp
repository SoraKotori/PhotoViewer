#include "decode_scheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <new>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace
{
using Microsoft::WRL::ComPtr;

struct ComScope
{
    ComScope()
        : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~ComScope()
    {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }

    HRESULT result;
};

[[nodiscard]] std::size_t expectedDecodedBytes(const std::filesystem::path& path)
{
    constexpr std::size_t fallback = 128ULL * 1024ULL * 1024ULL;
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 24> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        !std::equal(signature.begin(), signature.end(), header.begin()) ||
        header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') {
        return fallback;
    }
    const auto bigEndian = [&](const std::size_t offset) {
        return (static_cast<std::uint32_t>(header[offset]) << 24U) |
            (static_cast<std::uint32_t>(header[offset + 1]) << 16U) |
            (static_cast<std::uint32_t>(header[offset + 2]) << 8U) |
            static_cast<std::uint32_t>(header[offset + 3]);
    };
    const std::uint64_t width = bigEndian(16);
    const std::uint64_t height = bigEndian(20);
    if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / 4ULL / height) {
        return fallback;
    }
    return static_cast<std::size_t>(width * height * 4ULL);
}

[[nodiscard]] std::shared_ptr<DecodedImage> decodeImage(
    IWICImagingFactory* factory,
    const std::filesystem::path& path,
    HRESULT& error)
{
    const auto start = std::chrono::steady_clock::now();
    ComPtr<IWICBitmapDecoder> decoder;
    error = factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(error)) {
        return {};
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    error = decoder->GetFrame(0, &frame);
    if (FAILED(error)) {
        return {};
    }

    UINT width = 0;
    UINT height = 0;
    error = frame->GetSize(&width, &height);
    if (FAILED(error) || width == 0 || height == 0) {
        if (SUCCEEDED(error)) {
            error = E_INVALIDARG;
        }
        return {};
    }

    constexpr std::uint64_t bytesPerPixel = 4;
    const std::uint64_t stride64 = static_cast<std::uint64_t>(width) * bytesPerPixel;
    const std::uint64_t size64 = stride64 * static_cast<std::uint64_t>(height);
    if (stride64 > std::numeric_limits<UINT>::max() ||
        size64 > std::numeric_limits<UINT>::max() ||
        size64 > std::numeric_limits<std::size_t>::max()) {
        error = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        return {};
    }

    auto image = std::make_shared<DecodedImage>();
    image->path = path;
    image->width = width;
    image->height = height;
    try {
        image->pixels.resize(static_cast<std::size_t>(size64));
    } catch (const std::bad_alloc&) {
        error = E_OUTOFMEMORY;
        return {};
    }

    ComPtr<IWICFormatConverter> converter;
    error = factory->CreateFormatConverter(&converter);
    if (FAILED(error)) {
        return {};
    }
    error = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(error)) {
        return {};
    }

    const auto copyStart = std::chrono::steady_clock::now();
    image->setupMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(copyStart - start).count());
    error = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(stride64),
        static_cast<UINT>(size64),
        image->pixels.data());
    if (FAILED(error)) {
        return {};
    }

    const auto finish = std::chrono::steady_clock::now();
    image->copyMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(finish - copyStart).count());
    image->decodeMicroseconds = image->setupMicroseconds + image->copyMicroseconds;
    return image;
}
} // namespace

DecodeScheduler::DecodeScheduler(Telemetry& telemetry)
    : telemetry_(telemetry)
{
}

DecodeScheduler::~DecodeScheduler()
{
    stop();
}

void DecodeScheduler::start(
    const HWND notificationWindow,
    std::vector<std::filesystem::path> catalog,
    const std::size_t initialIndex,
    const std::size_t workerCount)
{
    stop();
    std::vector<std::size_t> decodedBytes;
    decodedBytes.reserve(catalog.size());
    for (const std::filesystem::path& path : catalog) {
        decodedBytes.push_back(expectedDecodedBytes(path));
    }
    {
        std::scoped_lock lock(mutex_);
        notificationWindow_ = notificationWindow;
        catalog_ = std::move(catalog);
        expectedDecodedBytes_ = std::move(decodedBytes);
        taskVersions_.assign(catalog_.size(), 0);
        stopping_ = false;
    }

    const std::size_t count = std::max<std::size_t>(1, workerCount);
    {
        std::scoped_lock lock(mutex_);
        workerCount_ = count;
        if (initialIndex < expectedDecodedBytes_.size()) {
            constexpr std::size_t bytesPerMicrosecondOfStagger = 6ULL * 1024ULL;
            workerStaggerMicroseconds_ = std::clamp<std::uint64_t>(
                expectedDecodedBytes_[initialIndex] / bytesPerMicrosecondOfStagger,
                2'000ULL,
                25'000ULL);
        } else {
            workerStaggerMicroseconds_ = 5'000;
        }
    }
    workers_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        workers_.emplace_back(&DecodeScheduler::workerLoop, this, index);
    }
}

void DecodeScheduler::stop()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    std::scoped_lock lock(mutex_);
    tasks_ = {};
    desiredPriorities_.clear();
    taskVersions_.clear();
    expectedDecodedBytes_.clear();
    inFlight_.clear();
    failed_.clear();
    cache_.clear();
    foregroundIndex_.reset();
    cacheBytes_ = 0;
    notificationWindow_ = nullptr;
}

void DecodeScheduler::updateDemand(
    const std::size_t displayedIndex,
    const std::optional<std::size_t> foregroundIndex,
    const int direction,
    const bool includeWorkerPipeline)
{
    HWND readyWindow = nullptr;
    std::optional<std::size_t> readyIndex;
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || displayedIndex >= catalog_.size() ||
            (foregroundIndex && *foregroundIndex >= catalog_.size())) {
            return;
        }

        displayedIndex_ = displayedIndex;
        const std::optional<std::size_t> previousForeground = foregroundIndex_;
        foregroundIndex_ = foregroundIndex;

        std::unordered_map<std::size_t, std::uint32_t> nextPriorities;
        std::size_t plannedBytes = 0;
        std::size_t pipelineImages = 0;
        constexpr std::size_t maximumPlannedImages = 128;
        const auto add = [&](const std::int64_t candidate, const std::uint32_t priority, const bool required = false) {
            if (candidate < 0 || candidate >= static_cast<std::int64_t>(catalog_.size())) {
                return false;
            }
            const std::size_t index = static_cast<std::size_t>(candidate);
            const auto existing = nextPriorities.find(index);
            if (existing != nextPriorities.end()) {
                auto entry = existing;
                entry->second = std::min(entry->second, priority);
                return true;
            }
            const std::size_t bytes = expectedDecodedBytes_.at(index);
            if (!required) {
                if (nextPriorities.size() >= maximumPlannedImages) {
                    return false;
                }
                if (bytes > cacheBudgetBytes_ - std::min(plannedBytes, cacheBudgetBytes_)) {
                    if (!includeWorkerPipeline || pipelineImages >= workerCount_) {
                        return false;
                    }
                    ++pipelineImages;
                }
            }
            nextPriorities.emplace(index, priority);
            plannedBytes += bytes;
            return true;
        };

        static_cast<void>(add(static_cast<std::int64_t>(displayedIndex), 1, true));
        if (foregroundIndex_) {
            static_cast<void>(add(static_cast<std::int64_t>(*foregroundIndex_), 0, true));
        }

        const std::size_t anchor = foregroundIndex_.value_or(displayedIndex_);
        const int primaryDirection = direction < 0 ? -1 : (direction > 0 ? 1 : 0);
        if (primaryDirection != 0) {
            constexpr std::size_t reverseDepth = 3;
            for (std::size_t distance = 1; distance <= reverseDepth; ++distance) {
                static_cast<void>(add(
                    static_cast<std::int64_t>(anchor) +
                        static_cast<std::int64_t>(-primaryDirection) * static_cast<std::int64_t>(distance),
                    static_cast<std::uint32_t>(3'000'000 + distance)));
            }
            for (std::size_t distance = 1; distance < catalog_.size(); ++distance) {
                const std::int64_t candidate = static_cast<std::int64_t>(anchor) -
                    static_cast<std::int64_t>(-primaryDirection) * static_cast<std::int64_t>(distance);
                if (!add(candidate, static_cast<std::uint32_t>(10 + distance))) {
                    break;
                }
            }
        } else {
            for (std::size_t distance = 1; distance < catalog_.size(); ++distance) {
                const std::int64_t forward =
                    static_cast<std::int64_t>(anchor) + static_cast<std::int64_t>(distance);
                const std::int64_t backward =
                    static_cast<std::int64_t>(anchor) - static_cast<std::int64_t>(distance);
                const bool forwardAdded = add(
                    forward,
                    static_cast<std::uint32_t>(10 + distance * 2));
                const bool backwardAdded = add(
                    backward,
                    static_cast<std::uint32_t>(11 + distance * 2));
                if (!forwardAdded && !backwardAdded) {
                    break;
                }
            }
        }

        for (const auto& [index, oldPriority] : desiredPriorities_) {
            const auto replacement = nextPriorities.find(index);
            if (replacement == nextPriorities.end() || replacement->second != oldPriority) {
                ++taskVersions_.at(index);
            }
        }

        const auto oldPriorities = desiredPriorities_;
        desiredPriorities_ = std::move(nextPriorities);
        for (const auto& [index, priority] : desiredPriorities_) {
            const auto previous = oldPriorities.find(index);
            const bool changed = previous == oldPriorities.end() || previous->second != priority;
            if (changed && !cache_.contains(index) && !inFlight_.contains(index) && !failed_.contains(index)) {
                tasks_.push(Task{
                    index,
                    priority,
                    taskVersions_.at(index),
                    ++taskSequence_});
            }
        }

        if (foregroundIndex_) {
            const auto cached = cache_.find(*foregroundIndex_);
            if (cached != cache_.end() && previousForeground != foregroundIndex_) {
                cached->second.lastUse = ++useClock_;
                readyWindow = notificationWindow_;
                readyIndex = *foregroundIndex_;
            } else if (failed_.contains(*foregroundIndex_)) {
                PostMessageW(
                    notificationWindow_,
                    kDecodeFailedMessage,
                    static_cast<WPARAM>(*foregroundIndex_),
                    0);
            }
        }
        trimCacheLocked();
    }

    condition_.notify_all();
    if (readyWindow != nullptr && readyIndex) {
        PostMessageW(readyWindow, kDecodeReadyMessage, static_cast<WPARAM>(*readyIndex), 0);
    }
}

std::shared_ptr<const DecodedImage> DecodeScheduler::tryGet(const std::size_t index)
{
    std::scoped_lock lock(mutex_);
    const auto found = cache_.find(index);
    if (found == cache_.end()) {
        return {};
    }
    found->second.lastUse = ++useClock_;
    return found->second.image;
}

DecodeInventorySnapshot DecodeScheduler::inventory(
    const std::size_t displayedIndex,
    const int direction) const
{
    std::scoped_lock lock(mutex_);
    DecodeInventorySnapshot snapshot{};
    snapshot.cachedImages = cache_.size();
    snapshot.inFlightImages = inFlight_.size();
    snapshot.desiredImages = desiredPriorities_.size();
    snapshot.cacheBytes = cacheBytes_;
    if (direction == 0 || displayedIndex >= catalog_.size()) {
        return snapshot;
    }
    const std::int64_t normalizedDirection = direction < 0 ? -1 : 1;
    for (std::size_t distance = 1; distance < catalog_.size(); ++distance) {
        const std::int64_t candidate = static_cast<std::int64_t>(displayedIndex) +
            normalizedDirection * static_cast<std::int64_t>(distance);
        if (candidate < 0 || candidate >= static_cast<std::int64_t>(catalog_.size()) ||
            !cache_.contains(static_cast<std::size_t>(candidate))) {
            break;
        }
        ++snapshot.readyAhead;
    }
    return snapshot;
}

std::size_t DecodeScheduler::catalogSize() const noexcept
{
    std::scoped_lock lock(mutex_);
    return catalog_.size();
}

const std::filesystem::path& DecodeScheduler::pathAt(const std::size_t index) const
{
    return catalog_.at(index);
}

bool DecodeScheduler::TaskCompare::operator()(const Task& left, const Task& right) const noexcept
{
    if (left.priority != right.priority) {
        return left.priority > right.priority;
    }
    return left.sequence > right.sequence;
}

bool DecodeScheduler::taskIsCurrentLocked(const Task& task) const
{
    const auto desired = desiredPriorities_.find(task.index);
    return desired != desiredPriorities_.end() &&
        desired->second == task.priority &&
        task.index < taskVersions_.size() &&
        taskVersions_[task.index] == task.version &&
        !cache_.contains(task.index) &&
        !inFlight_.contains(task.index) &&
        !failed_.contains(task.index);
}

std::optional<DecodeScheduler::Task> DecodeScheduler::takeForegroundTaskLocked()
{
    if (!foregroundIndex_ || cache_.contains(*foregroundIndex_) ||
        inFlight_.contains(*foregroundIndex_) || failed_.contains(*foregroundIndex_)) {
        return {};
    }
    const auto desired = desiredPriorities_.find(*foregroundIndex_);
    if (desired == desiredPriorities_.end()) {
        return {};
    }

    Task task{*foregroundIndex_, 0, taskVersions_.at(*foregroundIndex_), ++taskSequence_};
    inFlight_.insert(task.index);
    return task;
}

std::optional<DecodeScheduler::Task> DecodeScheduler::takeBackgroundTaskLocked()
{
    while (!tasks_.empty()) {
        const Task task = tasks_.top();
        if (!taskIsCurrentLocked(task)) {
            tasks_.pop();
            continue;
        }
        if (foregroundIndex_ && task.index == *foregroundIndex_) {
            return {};
        }
        tasks_.pop();
        inFlight_.insert(task.index);
        return task;
    }
    return {};
}

void DecodeScheduler::workerLoop(const std::size_t workerSlot)
{
    if (workerSlot != 0) {
        const std::uint64_t delayMicroseconds = workerStaggerMicroseconds_ * workerSlot;
        telemetry_.emit("worker_stagger", workerSlot, delayMicroseconds, workerStaggerMicroseconds_);
        std::unique_lock lock(mutex_);
        if (condition_.wait_for(
                lock,
                std::chrono::microseconds(delayMicroseconds),
                [this] { return stopping_; })) {
            return;
        }
    }

    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) {
        telemetry_.emit("worker_com_failed", 0, static_cast<std::uint32_t>(com.result));
        return;
    }

    ComPtr<IWICImagingFactory> factory;
    const HRESULT factoryResult = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(factoryResult)) {
        telemetry_.emit("worker_wic_failed", 0, static_cast<std::uint32_t>(factoryResult));
        return;
    }

    while (true) {
        std::optional<Task> task;
        bool foregroundTask = false;
        std::filesystem::path path;
        {
            std::unique_lock lock(mutex_);
            while (!stopping_ && !task) {
                task = takeForegroundTaskLocked();
                foregroundTask = task.has_value();
                if (!task) {
                    task = takeBackgroundTaskLocked();
                }
                if (!task) {
                    condition_.wait(lock);
                }
            }
            if (stopping_) {
                return;
            }
            path = catalog_.at(task->index);
        }
        condition_.notify_all();

        telemetry_.emit(
            "decode_start",
            task->index,
            task->priority,
            foregroundTask ? 1U : 0U,
            path);
        HRESULT decodeResult = E_FAIL;
        std::shared_ptr<DecodedImage> image = decodeImage(factory.Get(), path, decodeResult);
        finishDecode(*task, std::move(image), decodeResult);
    }
}

void DecodeScheduler::finishDecode(
    const Task& task,
    std::shared_ptr<DecodedImage> image,
    const HRESULT result)
{
    HWND window = nullptr;
    bool notifyReady = false;
    bool notifyFailed = false;
    std::filesystem::path path;
    std::uint64_t decodeMicroseconds = 0;
    std::uint64_t setupMicroseconds = 0;
    std::uint64_t copyMicroseconds = 0;
    std::size_t decodedBytes = 0;
    {
        std::scoped_lock lock(mutex_);
        inFlight_.erase(task.index);
        if (task.index < catalog_.size()) {
            path = catalog_.at(task.index);
        }
        if (image) {
            decodeMicroseconds = image->decodeMicroseconds;
            setupMicroseconds = image->setupMicroseconds;
            copyMicroseconds = image->copyMicroseconds;
            decodedBytes = image->pixels.size();
            const auto existing = cache_.find(task.index);
            if (existing != cache_.end()) {
                cacheBytes_ -= existing->second.image->pixels.size();
            }
            cache_[task.index] = CacheEntry{std::move(image), ++useClock_};
            cacheBytes_ += decodedBytes;
            trimCacheLocked();
            notifyReady = foregroundIndex_ && *foregroundIndex_ == task.index;
        } else if (!image) {
            failed_.insert(task.index);
            notifyFailed = foregroundIndex_ && *foregroundIndex_ == task.index;
        }
        if (notifyReady || notifyFailed) {
            window = notificationWindow_;
        }
    }

    if (decodedBytes != 0) {
        telemetry_.emit("decode_done", task.index, decodeMicroseconds, decodedBytes, path);
        telemetry_.emit("decode_stages", task.index, setupMicroseconds, copyMicroseconds, path);
    } else {
        telemetry_.emit("decode_failed", task.index, static_cast<std::uint32_t>(result), 0, path);
    }
    condition_.notify_all();
    if (window != nullptr) {
        PostMessageW(
            window,
            notifyReady ? kDecodeReadyMessage : kDecodeFailedMessage,
            static_cast<WPARAM>(task.index),
            0);
    }
}

void DecodeScheduler::trimCacheLocked()
{
    while (cacheBytes_ > cacheBudgetBytes_ && cache_.size() > 1) {
        auto victim = cache_.end();
        bool victimIsDesired = true;
        std::uint32_t victimPriority = 0;
        for (auto candidate = cache_.begin(); candidate != cache_.end(); ++candidate) {
            if (candidate->first == displayedIndex_ ||
                (foregroundIndex_ && candidate->first == *foregroundIndex_)) {
                continue;
            }
            const auto desired = desiredPriorities_.find(candidate->first);
            const bool candidateIsDesired = desired != desiredPriorities_.end();
            const std::uint32_t candidatePriority = candidateIsDesired ? desired->second : 0;
            const bool betterVictim = victim == cache_.end() ||
                (victimIsDesired && !candidateIsDesired) ||
                (victimIsDesired == candidateIsDesired && candidateIsDesired &&
                    candidatePriority > victimPriority) ||
                (victimIsDesired == candidateIsDesired &&
                    candidatePriority == victimPriority &&
                    candidate->second.lastUse < victim->second.lastUse);
            if (betterVictim) {
                victim = candidate;
                victimIsDesired = candidateIsDesired;
                victimPriority = candidatePriority;
            }
        }
        if (victim == cache_.end()) {
            break;
        }
        cacheBytes_ -= victim->second.image->pixels.size();
        cache_.erase(victim);
    }
}
