#include "storage_pipeline.h"

#include "storage_shutdown.h"

namespace pv {

void StoragePipeline::Shutdown() noexcept {
    constexpr std::uint64_t shutdown_timeout_ms = 5000;
    const StorageShutdownResult drained = DrainStorageForShutdown(
        model_.FrameCount(),
        [&](const std::size_t index) { return frames_.Io(index); },
        transport_, [] { return GetTickCount64(); }, shutdown_timeout_ms);
    if (drained == StorageShutdownResult::AbandonBacking) {
        transport_.AbandonForProcessExit();
        backing_.AbandonForProcessExit();
        shutdown_ = true;
        return;
    }
    for (std::size_t index = 0; index < model_.FrameCount(); ++index) {
        IoRequest* const request = frames_.Io(index);
        if (!request) continue;
        request->file.Reset();
        if (request->compressed_slot != kInvalidSlot) {
            slots_.ReleaseCompressed(request->compressed_slot);
            frames_.ClearCompressedSlot(index, request->compressed_slot);
        }
        frames_.DetachIo(index, request);
    }
    shutdown_ = true;
}

void StoragePipeline::RetireRead(const std::size_t frame) {
    const ImageRecord& image = frames_.View(frame);
    IoRequest* const request = frames_.Io(frame);
    if (!request) return;
    slots_.CancelFileRead(image.CompressedSlot());
    (void)transport_.RequestCancellation(*request);
}

void StoragePipeline::RemapActiveRead(const std::size_t destination) {
    IoRequest* const request = frames_.Io(destination);
    if (request) request->index = destination;
}

}  // namespace pv
