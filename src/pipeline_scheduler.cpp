#include "app.h"

#include "common.h"

namespace pv {

void PipelineRuntime::PumpPipeline() {
    if (state_.images.empty()) return;
    const auto begin = validation_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    for (int pass = 0; pass < 3; ++pass) {
        ReconcileReservations();
        validation_.Measure(TimedOperation::DispatchDecode,
                            [&] { DispatchDecodes(); });
        validation_.Measure(TimedOperation::SubmitReads,
                            [&] { SubmitReads(); });
        validation_.Measure(TimedOperation::SubmitUploads,
                            [&] { SubmitUploads(); });
        if (!validation_.Measure(TimedOperation::TryPresent,
                                 [&] { return TryPresent(); })) {
            break;
        }
    }
    validation_.Record(TimedOperation::PipelinePump, begin);
}

PipelineStage PipelineRuntime::StageOf(const ImageRecord& image) const noexcept {
    const std::size_t frame = static_cast<std::size_t>(
        &image - state_.images.data());
    if (image.failed) return PipelineStage::Failed;
    if (image.gpu_texture_reservation != kInvalidReservation &&
        image.gpu_texture_reservation < state_.slots.GpuTextureCount()) {
        const GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(
            image.gpu_texture_reservation);
        if (gpu_texture.reserved_frame == frame) {
            switch (gpu_texture.state) {
            case GpuTextureSlotState::Writing:
                return PipelineStage::Uploading;
            case GpuTextureSlotState::Readable:
            case GpuTextureSlotState::Reading:
                return PipelineStage::PresentationTextureAvailable;
            case GpuTextureSlotState::Writable:
            case GpuTextureSlotState::Inactive:
                break;
            }
        }
    }
    if (image.staging_slot != kInvalidSlot) {
        switch (state_.slots.StagingAt(image.staging_slot).state) {
            case StagingSlotState::Prepared:
                break;
            case StagingSlotState::DecodeOutputActive:
                return PipelineStage::DecodeQueued;
            case StagingSlotState::DecodedPixelsAvailable:
                return PipelineStage::DecodedStagingAvailable;
            case StagingSlotState::GpuCopySource:
                return PipelineStage::Uploading;
            case StagingSlotState::CancellationPending:
                return PipelineStage::CancelPending;
            case StagingSlotState::Free:
                break;
        }
    }
    if (image.compressed_slot != kInvalidSlot) {
        switch (state_.slots.Compressed(image.compressed_slot).state) {
            case CompressedSlotState::FileReadDestination:
                return PipelineStage::IoInFlight;
            case CompressedSlotState::CompressedDataAvailable:
                return PipelineStage::CompressedReady;
            case CompressedSlotState::DecodeInput:
                return PipelineStage::DecodeQueued;
            case CompressedSlotState::CancellationPending:
                return PipelineStage::CancelPending;
            case CompressedSlotState::Free:
                break;
        }
    }
    return ReservationActive(state_.reservations.Compressed(),
                             image.compressed_reservation, frame)
               ? PipelineStage::WaitingIo
               : PipelineStage::Outside;
}

void PipelineRuntime::InitializeReservations() {
    const std::size_t frame_count = state_.images.size();
    if (frame_count == 0) return;

    std::uintmax_t largest_file = 1;
    for (const CatalogItem& item : state_.catalog.items) {
        if (item.file_bytes != 0 && item.file_bytes <= config_.compressed_budget_bytes) {
            largest_file = std::max(largest_file, item.file_bytes);
        }
    }
    constexpr std::size_t decoded_8k_bytes = 7680ULL * 4320ULL * 4ULL;
    constexpr std::size_t staging_8k_bytes =
        decoded_8k_bytes + 4320ULL + 7680ULL * 4ULL;
    const auto fixed_capacity = [frame_count](const std::size_t slots,
                                               const std::size_t budget,
                                               const std::size_t unit) {
        return std::min({frame_count, slots, std::max<std::size_t>(1, budget / unit)});
    };
    const std::size_t compressed_capacity = fixed_capacity(
        config_.compressed_slot_count, config_.compressed_budget_bytes,
        static_cast<std::size_t>(largest_file));
    const std::size_t staging_capacity = fixed_capacity(
        config_.staging_slot_count, config_.staging_cache_bytes,
        staging_8k_bytes);
    const std::size_t gpu_texture_capacity = fixed_capacity(
        config_.GpuSlotCount(), config_.gpu_cache_bytes,
        decoded_8k_bytes);

    state_.reservations.Reset(compressed_capacity, staging_capacity,
                                  gpu_texture_capacity);
    for (SlotId id = 0; id < gpu_texture_capacity; ++id) {
        if (!state_.slots.ActivateGpuTexture(id)) {
            throw std::logic_error("failed to activate GPU Texture slot");
        }
    }
}

bool PipelineRuntime::ReservationActive(const ReservationTable& table,
                            const ReservationId id,
                            const std::size_t frame) const noexcept {
    return table.IsActive(id) && table.At(id).frame == frame;
}

void PipelineRuntime::RebuildReservationPlan() {
    const auto begin = validation_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    state_.reservations.Rebuild(state_.navigation, state_.images,
                                    work_queue_);
    validation_.Record(TimedOperation::ReservationPlan, begin);
}

void PipelineRuntime::ReconcileReservations() {
    const auto begin = validation_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    if (state_.reservations.NeedsRebuild(state_.images)) {
        RebuildReservationPlan();
    }

    const auto append_unique = [](std::vector<std::size_t>& frames,
                                  const std::size_t frame,
                                  const std::size_t capacity) {
        if (frames.size() < capacity &&
            std::find(frames.begin(), frames.end(), frame) == frames.end()) {
            frames.push_back(frame);
        }
    };
    const std::vector<std::size_t>& gpu_texture_desired =
        state_.reservations.DesiredGpuTextures();

    state_.reservations.GpuTextures().Reconcile(
        gpu_texture_desired,
        [&](const ReservationId id, const std::size_t) {
            const GpuTextureSlotState state = state_.slots.GpuTextureAt(id).state;
            return state == GpuTextureSlotState::Writable ||
                   state == GpuTextureSlotState::Readable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            if (image.gpu_texture_reservation == id) {
                image.gpu_texture_reservation = kInvalidReservation;
            }
            GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(id);
            gpu_texture.reserved_frame = kInvalidFrame;
            gpu_texture.state = GpuTextureSlotState::Writable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            image.gpu_texture_reservation = id;
            GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(id);
            gpu_texture.reserved_frame = frame;
            gpu_texture.generation = image.generation;
            gpu_texture.state = gpu_texture.content_frame == frame && gpu_texture.resource.bitmap
                               ? GpuTextureSlotState::Readable
                               : GpuTextureSlotState::Writable;
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            for (ReservationId id = 0; id < entries.size(); ++id) {
                if (entries[id].frame == kInvalidFrame &&
                    state_.slots.GpuTextureAt(id).content_frame == frame) {
                    return id;
                }
            }
            return ReservationTable::FirstFree(frame, entries);
        });

    std::vector<std::size_t>& staging_desired =
        state_.reservations.PrepareStagingDesired();
    for (const std::size_t frame : state_.reservations.PriorityOrder()) {
        if (staging_desired.size() ==
            state_.reservations.Staging().Capacity()) break;
        const ImageRecord& image = state_.images[frame];
        if (image.failed) continue;
        bool gpu_texture_complete = false;
        if (image.gpu_texture_reservation != kInvalidReservation) {
            const GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(
                image.gpu_texture_reservation);
            gpu_texture_complete = gpu_texture.reserved_frame == frame &&
                              gpu_texture.state != GpuTextureSlotState::Writable;
        }
        if (!gpu_texture_complete) append_unique(
            staging_desired, frame,
            state_.reservations.Staging().Capacity());
    }
    state_.reservations.Staging().Reconcile(
        staging_desired,
        [&](const ReservationId, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            if (image.staging_slot == kInvalidSlot) return true;
            StagingSlot& slot = state_.slots.StagingAt(image.staging_slot);
            if (slot.state == StagingSlotState::Prepared ||
                slot.state == StagingSlotState::DecodedPixelsAvailable) {
                return true;
            }
            if (slot.state == StagingSlotState::DecodeOutputActive ||
                slot.state == StagingSlotState::CancellationPending) {
                if (CancelQueuedDecode(frame)) return true;
                // The worker already owns this slot. Keep it alive and discard
                // or retain the result according to the reservation at completion.
                slot.state = StagingSlotState::CancellationPending;
            }
            return false;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            if (image.staging_slot != kInvalidSlot) {
                StagingSlot& slot = state_.slots.StagingAt(image.staging_slot);
                if (slot.state == StagingSlotState::Prepared ||
                    slot.state == StagingSlotState::DecodedPixelsAvailable) {
                    state_.slots.ReleaseStaging(image.staging_slot);
                    image.staging_slot = kInvalidSlot;
                }
            }
            if (image.staging_reservation == id) {
                image.staging_reservation = kInvalidReservation;
            }
        },
        [&](const ReservationId id, const std::size_t frame) {
            state_.images[frame].staging_reservation = id;
        },
        ReservationTable::FirstFree);

    std::vector<std::size_t>& compressed_desired =
        state_.reservations.PrepareCompressedDesired();
    for (const std::size_t frame : state_.reservations.PriorityOrder()) {
        if (compressed_desired.size() ==
            state_.reservations.Compressed().Capacity()) break;
        const PipelineStage stage = StageOf(state_.images[frame]);
        if (stage == PipelineStage::Failed ||
            stage == PipelineStage::DecodeQueued ||
            stage == PipelineStage::DecodedStagingAvailable ||
            stage == PipelineStage::Uploading ||
            stage == PipelineStage::PresentationTextureAvailable) {
            continue;
        }
        append_unique(compressed_desired, frame,
                      state_.reservations.Compressed().Capacity());
    }
    state_.reservations.Compressed().Reconcile(
        compressed_desired,
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            if (image.io) {
                // A submitted kernel I/O owns the compressed slot until its
                // completion packet arrives. Cancel only on the transition to
                // retiring; repeated pipeline passes keep waiting for the same
                // completion packet without reissuing the cancellation.
                state_.slots.Compressed(image.compressed_slot).state =
                    CompressedSlotState::CancellationPending;
                if (!state_.reservations.Compressed().At(id).retiring &&
                    !CancelIoEx(image.io->file.Get(), nullptr)) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_NOT_FOUND) {
                        SetLastError(error);
                        ThrowLastError("CancelIoEx(retired image)");
                    }
                }
                return false;
            }
            if (image.compressed_slot == kInvalidSlot) return true;
            return state_.slots.Compressed(image.compressed_slot).state ==
                   CompressedSlotState::CompressedDataAvailable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            if (image.compressed_slot != kInvalidSlot) ReleaseCompressed(image);
            if (image.compressed_reservation == id) {
                image.compressed_reservation = kInvalidReservation;
            }
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = state_.images[frame];
            image.compressed_reservation = id;
            const CatalogItem& item = state_.catalog.items[frame];
            image.failed = item.file_size_known && item.file_bytes == 0;
        },
        ReservationTable::FirstFree);
    validation_.Record(TimedOperation::ReservationReconcile, begin);
}

}  // namespace pv





