#include "config.h"
#include "navigation.h"
#include "png.h"
#include "processor_topology.h"
#include "pipeline_resource_size.h"
#include "pipeline_model.h"
#include "pipeline_resources.h"
#include "pipeline_state.h"
#include "presentation_order.h"
#include "reservation_planner.h"
#include "resource_slots.h"
#include "reservation.h"
#include "runtime_telemetry.h"
#include "upload_ledger.h"
#include "work_queue.h"

#include "../third_party/libdeflate/libdeflate.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

template <typename Access>
concept CanBeginDecodeWork = requires(Access access) { access.BeginWork(0); };
template <typename Access>
concept CanAttachCompressed = requires(Access access) {
    access.AttachCompressedSlot(0, 0);
};
template <typename Access>
concept CanClearStaging = requires(Access access) {
    access.ClearStagingSlot(0, 0);
};
template <typename Access>
concept CanReadIoRequest = requires(Access access) {
    access.FileReadRequest(0);
};
template <typename Access>
concept CanMutateGpu = requires(Access access) { access.BeginGpuUpload(0); };
template <typename Access>
concept CanRecordResourcePlan = requires(
    Access access, const pv::PngResourcePlan& plan) {
    access.RecordResourcePlan(0, plan);
};
template <typename Access>
concept CanCompletePresentation = requires(Access access) {
    access.Complete(0);
};
template <typename Model>
concept CanNavigate = requires(Model& model) { model.Navigate(1, false); };

static_assert(CanAttachCompressed<pv::StorageFrameAccess>);
static_assert(!CanBeginDecodeWork<pv::StorageFrameAccess>);
static_assert(!CanClearStaging<pv::StorageFrameAccess>);
static_assert(CanBeginDecodeWork<pv::DecodeFrameAccess>);
static_assert(!CanAttachCompressed<pv::DecodeFrameAccess>);
static_assert(CanClearStaging<pv::GraphicsFrameAccess>);
static_assert(!CanBeginDecodeWork<pv::GraphicsFrameAccess>);
static_assert(CanReadIoRequest<pv::StorageResourceAccess>);
static_assert(!CanMutateGpu<pv::StorageResourceAccess>);
static_assert(!CanReadIoRequest<pv::DecodeResourceAccess>);
static_assert(!CanMutateGpu<pv::DecodeResourceAccess>);
static_assert(CanMutateGpu<pv::GraphicsResourceAccess>);
static_assert(!CanReadIoRequest<pv::GraphicsResourceAccess>);
static_assert(CanRecordResourcePlan<pv::StorageCatalogAccess>);
static_assert(!CanCompletePresentation<pv::StorageCatalogAccess>);
static_assert(CanCompletePresentation<pv::PresentationCompletionAccess>);
static_assert(!CanRecordResourcePlan<pv::PresentationCompletionAccess>);
static_assert(!CanNavigate<const pv::PipelineModel>);
static_assert(pv::ShouldContinuePipelinePass(true, false));
static_assert(pv::ShouldContinuePipelinePass(false, true));
static_assert(!pv::ShouldContinuePipelinePass(false, false));

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ConfigDefaultTests() {
    const pv::Config config;
    Check(config.worker_count == 0,
          "application default must select workers from physical cores");
    Check(config.compressed_slot_count == 10,
          "application default must retain two compressed read-ahead slots");
    Check(config.staging_slot_count == 12,
          "application default must retain four decoded-output pipeline slots");
    Check(config.gpu_forward_slot_count == 3 &&
              config.gpu_reverse_slot_count == 1,
          "application default must use the measured GPU slot minimum");
    Check(config.png_validation.chunk_crc == pv::PngChunkCrcMode::All &&
              config.png_validation.adler32,
          "PNG integrity validation must be strict by default");
}

std::size_t ProcessorTopologyTests() {
    Check(pv::DefaultWorkerCountForPhysicalCores(0) == 1,
          "missing topology must retain one worker");
    Check(pv::DefaultWorkerCountForPhysicalCores(1) == 1,
          "one physical core must select one worker");
    Check(pv::DefaultWorkerCountForPhysicalCores(8) == 8,
          "worker default must follow physical cores below the cap");
    Check(pv::DefaultWorkerCountForPhysicalCores(16) == 8,
          "worker default must stop at the eight-worker cap");
    Check(pv::DefaultWorkerCountForPhysicalCores(32) == 8,
          "worker default must not exceed the cap");

    const std::size_t detected = pv::DetectPhysicalCoreCount();
    Check(detected > 0, "physical core detection must return a usable count");
    Check(pv::DefaultWorkerCount() ==
              pv::DefaultWorkerCountForPhysicalCores(detected),
          "runtime worker default must use the detected physical core count");
    return detected;
}

std::size_t PresentNext(pv::NavigationState& navigation) {
    const auto next = navigation.NextIndex();
    Check(next.has_value(), "expected a pending presentation");
    navigation.CompletePresentation(*next);
    return *next;
}

void NavigationTests() {
    pv::NavigationState navigation;
    navigation.Reset(5, 20);
    Check(PresentNext(navigation) == 5, "initial image must be presented first");

    for (int count = 0; count < 5; ++count) navigation.Step(1, false);
    navigation.Step(-1, false);
    const std::array<std::size_t, 6> expected{6, 7, 8, 9, 10, 9};
    for (const std::size_t index : expected) {
        Check(PresentNext(navigation) == index, "short presses must preserve order");
    }
    Check(navigation.Empty(), "short press sequence must drain exactly");

    navigation.Reset(5, 20);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true);
    navigation.Step(1, true);
    navigation.Release(1);
    Check(PresentNext(navigation) == 6, "release must retain the initial committed press");
    Check(navigation.Empty(), "release must discard unpresented repeats");

    navigation.Reset(5, 20);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true);
    for (const std::size_t index : std::array<std::size_t, 2>{6, 7}) {
        Check(PresentNext(navigation) == index,
              "each repeat event must add exactly one adjacent image");
    }
    Check(navigation.Empty(), "single repeat event must drain exactly");

    navigation.Reset(5, 20);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true);
    navigation.Release(1);
    Check(PresentNext(navigation) == 6,
          "release must discard every unpresented merged repeat");
    Check(navigation.Empty(), "repeat event must not survive release");

    navigation.Reset(0, 2);
    PresentNext(navigation);
    navigation.Step(-1, false);
    Check(navigation.Empty(), "navigation must stop at folder boundary");
    navigation.Step(1, false);
    Check(PresentNext(navigation) == 1, "forward boundary step");
    navigation.Step(1, false);
    Check(navigation.Empty(), "upper boundary must not wrap");

    navigation.Reset(10, 30);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true);
    navigation.Step(-1, false);
    Check(PresentNext(navigation) == 11, "committed forward step survives reversal");
    Check(PresentNext(navigation) == 10, "committed reverse step follows in order");
    Check(navigation.Empty(), "old-direction repeat is discarded on reversal");

    navigation.Reset(10, 30);
    PresentNext(navigation);
    navigation.Step(-1, false);
    navigation.Release(-1);
    Check(PresentNext(navigation) == 9, "left short press must be presented");
    Check(navigation.Empty(), "left short press must drain exactly");
    Check(navigation.PreferredDirection() == -1,
          "idle prefetch must retain the last successful left direction");
    std::vector<std::size_t> left_plan;
    navigation.BuildPlan(4, left_plan);
    Check(left_plan == std::vector<std::size_t>({8, 7, 6, 5}),
          "planned order must extend from the requested left direction");
    navigation.Step(1, false);
    navigation.Release(1);
    Check(PresentNext(navigation) == 10, "right short press after left must be presented");
    Check(navigation.PreferredDirection() == 1,
          "idle prefetch must retain the last successful right direction");

    navigation.Reset(0, 30);
    PresentNext(navigation);
    navigation.Step(-1, false);
    navigation.Release(-1);
    Check(navigation.PreferredDirection() == 0,
          "a rejected boundary step must not replace the last effective direction");
}

void ReservationPlannerTests() {
    pv::Config config;
    config.compressed_slot_count = 4;
    config.staging_slot_count = 3;
    config.gpu_forward_slot_count = 2;
    config.gpu_reverse_slot_count = 1;

    pv::ReservationPlanner planner{pv::PipelineLimits(config)};
    planner.Reset(4, 3, 3);
    pv::NavigationState navigation;
    navigation.Reset(5, 12);
    PresentNext(navigation);
    navigation.Step(1, false);
    std::vector<pv::ImageRecord> images(12);
    planner.Rebuild(navigation, images);

    const auto& desired = planner.DesiredGpuTextures();
    Check(desired.size() == 3 && desired[0] == 5 &&
              desired[1] == 6 && desired[2] == 4,
          "planner must reserve current, forward deadline and reverse reuse");
    Check(planner.PriorityOrder().front() == 5,
          "upstream work must start with a GPU-backed frame");
    Check(!planner.NeedsRebuild(images),
          "stable image state must reuse the existing plan");

    images[6] = pv::ImageRecord::FailedRecord();
    Check(planner.NeedsRebuild(images),
          "a failed desired frame must invalidate the plan");
    planner.Rebuild(navigation, images);
    Check(std::find(planner.DesiredGpuTextures().begin(),
                    planner.DesiredGpuTextures().end(), 6) ==
              planner.DesiredGpuTextures().end(),
          "failed frames must be removed from GPU reservations");
}

std::array<std::byte, pv::kPngHeaderBytes> MakePngHeader(
    const std::uint32_t width, const std::uint32_t height,
    const std::uint8_t depth = 8, const std::uint8_t color = 6,
    const std::uint8_t interlace = 0) {
    std::array<std::byte, 33> header{};
    const std::array<unsigned char, 16> prefix{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        header[index] = std::byte{prefix[index]};
    }
    const auto write = [&](const std::size_t offset,
                           const std::uint32_t value) {
        header[offset] = std::byte{static_cast<std::uint8_t>(value >> 24U)};
        header[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 16U)};
        header[offset + 2] = std::byte{static_cast<std::uint8_t>(value >> 8U)};
        header[offset + 3] = std::byte{static_cast<std::uint8_t>(value)};
    };
    write(16, width);
    write(20, height);
    header[24] = std::byte{depth};
    header[25] = std::byte{color};
    header[28] = std::byte{interlace};
    write(29, libdeflate_crc32(0, header.data() + 12, 17));
    return header;
}

void PngTests() {
    auto header = MakePngHeader(7680, 4320);
    const auto parsed = pv::ParsePngResourcePlan(header);
    Check(parsed.has_value(), "valid PNG IHDR");
    Check(parsed->width == 7680 && parsed->height == 4320, "8K dimensions");
    Check(parsed->decoded_bytes == 7680ULL * 4320ULL * 4ULL,
          "decoded byte reservation");
    Check(parsed->filter_workspace_bytes == 4320 + 7680 * 4 &&
              parsed->staging_committed_bytes ==
                  parsed->decoded_bytes + parsed->filter_workspace_bytes &&
              parsed->texture_width == 7680 &&
              parsed->texture_height == 4321 &&
              parsed->gpu_reservation_bytes == parsed->decoded_bytes,
          "main-thread resource plan must contain all downstream sizes");
    const auto tall = pv::ParsePngResourcePlan(
        MakePngHeader(16383, 16384));
    Check(tall && tall->texture_width == 16384 &&
              tall->texture_height == 16384,
          "maximum-height texture may use the planned workspace column");
    Check(!pv::ParsePngResourcePlan(MakePngHeader(16384, 16384)),
          "maximum square texture has no room for PNG filter workspace");
    Check(!pv::ParsePngResourcePlan(MakePngHeader(16385, 1)) &&
              !pv::ParsePngResourcePlan(MakePngHeader(1, 16385)),
          "IHDR dimensions beyond D3D11 limits must fail before allocation");
    header[0] = std::byte{0};
    Check(!pv::ParsePngResourcePlan(header), "invalid signature must fail");
}

void ResourceSlotTests() {
    pv::ResourceSlots slots(2, 2, 2, 8192, 8192);
    Check(slots.CompressedCount() == 2 && slots.FreeCompressedCount() == 2,
          "compressed slot storage and free index must start at configured count");
    Check(slots.StagingCount() == 2 && slots.FreeStagingCount() == 2,
          "staging slot storage and free index must start at configured count");
    Check(slots.GpuTextureCount() == 2 &&
              slots.InactiveGpuTextureCount() == 2,
          "GPU slot storage and inactive index must start at configured count");

    const pv::SlotId compressed0 = slots.AcquireCompressed(4096, 3, 7);
    const pv::SlotId compressed1 = slots.AcquireCompressed(4096, 4, 7);
    Check(compressed0 != pv::kInvalidSlot && compressed1 != pv::kInvalidSlot,
          "configured compressed slots must be allocatable");
    Check(slots.FreeCompressedCount() == 0,
          "compressed free index must exclude occupied slots");
    Check(slots.AcquireCompressed(1, 5, 7) == pv::kInvalidSlot,
          "compressed slot count must be a hard limit");
    Check(slots.Compressed(compressed0).State() ==
              pv::CompressedSlotState::FileReadDestination,
          "compressed acquisition state must describe its pipeline phase");
    const pv::IoRequest* const io_address =
        &slots.Compressed(compressed0).Request();
    slots.CompleteFileRead(compressed0);
    slots.ReleaseCompressed(compressed0);
    Check(slots.FreeCompressedCount() == 1 &&
              slots.Compressed(compressed0).State() ==
                  pv::CompressedSlotState::Free,
          "compressed release must restore state and free index together");
    const pv::SlotId recycled_compressed = slots.AcquireCompressed(4096, 5, 8);
    Check(recycled_compressed == compressed0 &&
              &slots.Compressed(recycled_compressed).Request() == io_address,
          "compressed slot must retain an inline stable I/O request");
    slots.ReleaseCompressed(recycled_compressed);

    pv::ResourceSlots growing_slots(1, 1, 1, 8192, 4096);
    const pv::SlotId growing = growing_slots.AcquireCompressed(4096, 0, 1);
    Check(growing != pv::kInvalidSlot,
          "compressed slot must allocate initial storage");
    growing_slots.ReleaseCompressed(growing);
    Check(growing_slots.AcquireCompressed(8192, 1, 2) == growing &&
              growing_slots.Compressed(growing).Buffer().data != nullptr &&
              growing_slots.Compressed(growing).Buffer().allocation_size == 8192,
          "free compressed slot must grow its allocation immediately");
    growing_slots.ReleaseCompressed(growing);

    const pv::SlotId staging0 = slots.AcquireStaging(4096, 3, 7);
    const pv::SlotId staging1 = slots.AcquireStaging(4096, 4, 7);
    Check(staging0 != pv::kInvalidSlot && staging1 != pv::kInvalidSlot,
          "configured staging slots must be allocatable");
    Check(slots.AcquireStaging(1, 5, 7) == pv::kInvalidSlot,
          "staging slot count must be a hard limit");
    Check(slots.Staging(staging0).State() ==
              pv::StagingSlotState::Prepared,
          "staging acquisition must reserve an unmapped prepared slot");
    slots.BeginDecodeOutput(staging0);
    slots.CompleteDecodeOutput(staging0);
    slots.ReleaseStaging(staging0);
    Check(slots.FreeStagingCount() == 1 &&
              slots.Staging(staging0).State() ==
                  pv::StagingSlotState::Free,
          "staging release must restore state and free index together");
    const pv::SlotId recycled_staging = slots.AcquireStaging(4096, 5, 8);
    Check(recycled_staging == staging0,
          "staging slot index must be recycled without auxiliary token state");
    slots.ReleaseStaging(recycled_staging);

    constexpr pv::SlotId gpu0 = 0;
    constexpr pv::SlotId gpu1 = 1;
    Check(slots.ActivateGpuTexture(gpu0) && slots.ActivateGpuTexture(gpu1),
          "configured GPU Texture slots must be activatable once");
    Check(!slots.ActivateGpuTexture(2),
          "GPU Texture slot count must be a hard limit");
    Check(slots.GpuTexture(gpu0).State() ==
              pv::GpuTextureSlotState::Writable,
          "GPU acquisition state must describe its pipeline phase");
    Check(slots.InactiveGpuTextureCount() == 0,
          "fixed GPU Texture slots leave the inactive index when activated");
    slots.ReserveGpuTexture(gpu0, 3, 7);
    Check(slots.GpuTexture(gpu0).ReservedFrame() == 3 &&
              slots.GpuTexture(gpu0).ReservationGeneration() == 7,
          "GPU reservation must update frame identity atomically");
    slots.BeginGpuUpload(gpu0);
    slots.CompleteGpuUpload(gpu0, 3, 7, 90, true);
    Check(slots.GpuTexture(gpu0).ContentFrame() == 3 &&
              slots.GpuTexture(gpu0).ContentGeneration() == 7,
          "GPU completion must publish content identity in Writing state");
    bool rejected_invalid_transition = false;
    try {
        slots.CompleteGpuRead(gpu0);
    } catch (const std::logic_error&) {
        rejected_invalid_transition = true;
    }
    Check(rejected_invalid_transition,
          "slot lifecycle must reject a transition from an unexpected state");
    slots.ClearGpuTextureReservation(gpu0);
    Check(slots.GpuTexture(gpu0).ReservedFrame() == pv::kInvalidFrame &&
              slots.GpuTexture(gpu0).ContentFrame() == 3 &&
              slots.GpuTexture(gpu0).State() ==
                  pv::GpuTextureSlotState::Writable,
          "clearing a GPU reservation must retain reusable stale content");
    slots.ReserveGpuTexture(gpu0, 3, 8);
    Check(slots.GpuTexture(gpu0).ContentFrame() == 3 &&
              slots.GpuTexture(gpu0).ContentGeneration() == 7 &&
              slots.GpuTexture(gpu0).State() ==
                  pv::GpuTextureSlotState::Writable,
          "a new catalog generation must not reuse same-index GPU content");
    Check(slots.ReleaseReplaceableGpuContent(gpu0) == 90 &&
              slots.GpuTexture(gpu0).ReservedFrame() == 3 &&
              slots.GpuTexture(gpu0).ContentFrame() == pv::kInvalidFrame,
          "budget pressure must evict reserved-but-mismatched stale content");
    slots.ClearGpuTextureReservation(gpu0);
    Check(slots.ReleaseReplaceableGpuContent(gpu0) == 0 &&
              slots.GpuTexture(gpu0).ContentFrame() == pv::kInvalidFrame,
          "budget-pressure eviction must invalidate only unreserved GPU content");

    pv::ResourceSlots budget_limited(2, 2, 1, 4096, 4096);
    Check(budget_limited.AcquireCompressed(4096, 0, 1) != pv::kInvalidSlot,
          "compressed byte budget must allow one exact allocation");
    Check(budget_limited.AcquireCompressed(1, 1, 1) == pv::kInvalidSlot,
          "compressed byte budget and slot count must both be enforced");
    Check(budget_limited.AcquireStaging(4096, 0, 1) != pv::kInvalidSlot,
          "staging byte budget must allow one exact allocation");
    Check(budget_limited.AcquireStaging(1, 1, 1) == pv::kInvalidSlot,
          "staging byte budget and slot count must both be enforced");
}

void ReservationTests() {
    pv::ReservationTable table;
    table.Reset(2);
    std::vector<std::size_t> released;
    std::vector<std::size_t> assigned;
    const auto reconcile = [&](const std::vector<std::size_t>& desired,
                               const bool allow_release) {
        table.Reconcile(
            desired,
            [&](pv::ReservationId, std::size_t) { return allow_release; },
            [&](pv::ReservationId, std::size_t frame) { released.push_back(frame); },
            [&](pv::ReservationId, std::size_t frame) { assigned.push_back(frame); },
            pv::ReservationTable::FirstFree);
    };
    reconcile({10, 11}, true);
    Check(table.AssignedCount() == 2 && table.FindFrame(10) != pv::kInvalidReservation,
          "reservation table must assign fixed capacity in desired order");
    reconcile({12, 11}, false);
    const auto old = table.FindFrame(10);
    Check(old != pv::kInvalidReservation && table.At(old).retiring,
          "busy obsolete reservation must retire without unsafe reuse");
    reconcile({10, 11}, false);
    Check(table.FindFrame(10) == old && table.At(old).retiring,
          "a desired frame must not revoke an in-flight cancellation");
    reconcile({10, 11}, true);
    Check(table.FindFrame(10) != pv::kInvalidReservation &&
              !table.At(table.FindFrame(10)).retiring,
          "a completed cancellation must release and reacquire the reservation");
    reconcile({12, 11}, false);
    reconcile({12, 11}, true);
    Check(table.FindFrame(12) != pv::kInvalidReservation &&
              table.FindFrame(10) == pv::kInvalidReservation,
          "safe retired reservation must be reassigned without a second pool");

    table.Reset(3);
    reconcile({10, 11, 12}, true);
    const std::array<std::size_t, 3> independently_reassigned{20, 21, 12};
    table.Reconcile(
        independently_reassigned,
        [](pv::ReservationId, const std::size_t frame) { return frame != 10; },
        [](pv::ReservationId, std::size_t) {},
        [](pv::ReservationId, std::size_t) {},
        pv::ReservationTable::FirstFree);
    Check(table.IsRetiring(table.FindFrame(10)) &&
              table.FindFrame(20) != pv::kInvalidReservation,
          "one retiring owner must not block independently released capacity");

    pv::WorkQueue queue(4);
    pv::DecodeWork work{3, 7, 1, 2};
    Check(queue.TryPush(work), "decode work must enter queue");
    pv::DecodeWork cancelled;
    Check(queue.TryCancel(2, cancelled) && cancelled.index == 3,
          "unclaimed decode work must be synchronously cancellable");
    Check(queue.Size() == 0,
          "cancelled work must leave the queue exactly once");

    pv::DecodeWork popped;
    pv::DecodeWork claimed_work{5, 8, 1, 3};
    Check(queue.TryPush(claimed_work) &&
              queue.Pop(popped, std::stop_token{}),
          "worker must be able to claim queued work");
    Check(!queue.TryCancel(3, cancelled) && popped.index == 5,
          "claimed work must retain slot ownership until completion");

    pv::DecodeWork low{9, 1, 0, 0};
    pv::DecodeWork high{4, 1, 0, 1};
    Check(queue.TryPush(low) && queue.TryPush(high),
          "priority reorder test work must enter queue");
    queue.Reorder(std::array<std::size_t, 2>{4, 9});
    Check(queue.Pop(popped, std::stop_token{}) && popped.index == 4,
          "queued decode work must follow the latest navigation priority");

    pv::WorkQueue remap_queue(1);
    pv::DecodeWork remapped{0, 11, 0, 0};
    Check(remap_queue.TryPush(remapped), "catalog remap test work must enter queue");
    remap_queue.Remap(0, 84, 11);
    Check(remap_queue.Pop(popped, std::stop_token{}) && popped.index == 84,
          "queued decode work must follow the catalog frame remap");

    pv::WorkQueue naturally_bounded_queue(32);
    for (std::size_t index = 0; index < 32; ++index) {
        pv::DecodeWork queued{index, 1, 0, static_cast<pv::SlotId>(index)};
        Check(naturally_bounded_queue.TryPush(queued),
              "slot-backed work must fit the queue's fixed construction capacity");
    }
    Check(naturally_bounded_queue.Size() == 32,
          "work queue must retain every slot-backed item without reallocating");
    pv::DecodeWork overflow{32, 1, 0, 32};
    Check(!naturally_bounded_queue.TryPush(overflow),
          "work queue must reject work beyond its fixed slot capacity");
}

void CompletionQueueTests() {
    pv::CompletionQueue queue(2);
    Check(WaitForSingleObject(queue.CompletionEvent(), 0) == WAIT_TIMEOUT,
          "completion event must initially be clear");
    queue.PushReleasedInput(pv::ReleasedInput{3, 7, 1});
    Check(WaitForSingleObject(queue.CompletionEvent(), 0) == WAIT_OBJECT_0,
          "first completion must signal the event");
    queue.Push(pv::DecodeResult{3, 7, true, S_OK, 2});
    Check(WaitForSingleObject(queue.CompletionEvent(), 0) == WAIT_OBJECT_0,
          "coalesced completions must keep the event signaled");

    pv::CompletionQueue::Batch batch(2);
    queue.DrainAll(batch);
    Check(batch.released_inputs.size() == 1 && batch.results.size() == 1,
          "one critical section must drain both completion event classes");
    Check(WaitForSingleObject(queue.CompletionEvent(), 0) == WAIT_TIMEOUT,
          "draining must reset the completion event");
    queue.Push(pv::DecodeResult{4, 7, true, S_OK, 3});
    Check(WaitForSingleObject(queue.CompletionEvent(), 0) == WAIT_OBJECT_0,
          "a later completion must re-signal the event");
    queue.DrainAll(batch);
    Check(batch.results.size() == 1 && batch.results.front().index == 4,
          "completion queue must remain reusable after a batch drain");
}

void RuntimeTelemetryTests() {
    pv::RuntimeTelemetry telemetry(std::chrono::steady_clock::now());
    telemetry.BeginNavigation(std::chrono::steady_clock::now());
    const int measured = telemetry.Measure(
        pv::TimedOperation::PipelinePump, [] { return 42; });
    Check(measured == 42,
          "telemetry measurement must preserve operation results");
    Check(telemetry.Timing(pv::TimedOperation::PipelinePump).calls == 1,
          "measured operation must increment its call count");
    telemetry.Record(pv::TimedOperation::PipelinePump, {});
    Check(telemetry.Timing(pv::TimedOperation::PipelinePump).calls == 1,
          "zero begin time must not record process uptime as an operation");
}

void ReservationByteBudgetTests() {
    pv::CatalogItem compressed;
    compressed.file_size_known = true;
    compressed.file_bytes = 4097;
    const auto aligned = pv::CompressedReservationBytes(compressed, 1);
    Check(aligned && *aligned == 8192,
          "compressed reservations must include 4 KiB backing alignment");
    std::size_t used = 0;
    Check(pv::AddWithinBudget(*aligned, 8194, used),
          "first aligned compressed reservation must fit");
    Check(!pv::AddWithinBudget(*aligned, 8194, used),
          "raw file sizes must not overcommit aligned backing storage");
    Check(pv::CompressedReservationBytes(compressed, 1, 65536) == 65536,
          "compressed reservations must honor the transport allocation alignment");
    Check(pv::CompressedReservationBytes(compressed, 8192, 0) == 8192,
          "unknown transport alignment must conservatively reserve the budget");

    pv::CatalogItem decoded;
    decoded.header_valid = true;
    decoded.resource_plan = *pv::ParsePngResourcePlan(MakePngHeader(10, 2));
    const auto staging = pv::StagingReservationBytes(decoded, 1);
    const auto gpu = pv::GpuReservationBytes(decoded, 1);
    Check(staging && *staging == 122,
          "staging reservation must include filters and row scratch");
    Check(gpu && *gpu == 80,
          "GPU reservation must use decoded pixel bytes");

    pv::CatalogItem unknown;
    Check(pv::StagingReservationBytes(unknown, 777) == 777 &&
              pv::GpuReservationBytes(unknown, 888) == 888,
          "unknown headers must conservatively reserve the whole budget");

    pv::FixedSlotByteBudget residency(2, 100);
    Check(residency.CanReplace(0, 50),
          "first GPU slot must fit its fixed residency budget");
    residency.CommitReplacement(0, 50);
    residency.CommitReplacement(1, 50);
    Check(residency.Committed() == 100 && !residency.CanReplace(0, 100),
          "stale GPU residency must prevent an over-budget replacement");
    Check(residency.Release(0) == 50 && residency.Release(1) == 50 &&
              residency.CanReplace(0, 100),
          "releasing obsolete reservations must make the full budget redeemable");
    residency.CommitReplacement(0, 100);
    Check(residency.Committed() == 100,
          "a replacement after stale-slot reclamation must consume exact bytes");

    pv::ResourceSlots stale_slots(0, 0, 2, 0, 0);
    Check(stale_slots.ActivateGpuTexture(0) &&
              stale_slots.ActivateGpuTexture(1),
          "pressure test must activate both fixed GPU slots");
    stale_slots.ReserveGpuTexture(0, 0, 1);
    stale_slots.BeginGpuUpload(0);
    stale_slots.CompleteGpuUpload(0, 0, 1, 90, true);
    stale_slots.ReserveGpuTexture(1, 1, 1);
    stale_slots.BeginGpuUpload(1);
    stale_slots.CompleteGpuUpload(1, 1, 1, 10, true);
    pv::FixedSlotByteBudget swapped_residency(2, 100);
    swapped_residency.CommitReplacement(0, 90);
    swapped_residency.CommitReplacement(1, 10);
    stale_slots.ClearGpuTextureReservation(0);
    stale_slots.ClearGpuTextureReservation(1);
    stale_slots.ReserveGpuTexture(0, 1, 2);
    stale_slots.ReserveGpuTexture(1, 0, 2);
    Check(!swapped_residency.CanReplace(1, 90),
          "inverse-size readiness must initially encounter stale residency");
    const std::size_t evicted =
        stale_slots.ReleaseReplaceableGpuContent(0);
    Check(evicted == 90 && swapped_residency.Release(0) == evicted &&
              swapped_residency.CanReplace(1, 90) &&
              stale_slots.GpuTexture(0).ReservedFrame() == 1,
          "pressure eviction must preserve the reassigned reservation while "
          "making its peer immediately redeemable");
}

void ControlledCompletionOrderingTests() {
    pv::NavigationState navigation;
    navigation.Reset(0, 4);
    navigation.Step(1, false);
    navigation.Step(1, false);
    std::array<pv::PipelineStage, 4> stages{};
    stages.fill(pv::PipelineStage::Outside);

    pv::UploadLedger uploads(3);
    uploads.Queue(pv::UploadTicket{2, 1, 10});
    uploads.Queue(pv::UploadTicket{1, 1, 20});
    uploads.Queue(pv::UploadTicket{0, 1, 30});
    while (auto ticket = uploads.TakeCompleted(20)) {
        stages[ticket->index] = pv::PipelineStage::PresentationTextureAvailable;
    }
    const auto ready = [&](const std::size_t frame) {
        return stages[frame] ==
               pv::PipelineStage::PresentationTextureAvailable;
    };
    Check(!pv::NextPresentableFrame(navigation, ready),
          "later GPU completions must not bypass the first ordered frame");

    const auto first = uploads.TakeCompleted(30);
    Check(first && first->index == 0 && uploads.Empty(),
          "coalesced fence completion must release every prior upload");
    stages[first->index] = pv::PipelineStage::PresentationTextureAvailable;
    for (const std::size_t expected : std::array<std::size_t, 3>{0, 1, 2}) {
        const auto frame = pv::NextPresentableFrame(navigation, ready);
        Check(frame && *frame == expected,
              "out-of-order intermediate completion must present in navigation order");
        navigation.CompletePresentation(*frame);
    }

    navigation.Reset(2, 5);
    navigation.CompletePresentation(2);
    navigation.Step(1, false);
    navigation.Step(1, true);
    navigation.Release(1);
    navigation.Step(-1, false);
    stages.fill(pv::PipelineStage::PresentationTextureAvailable);
    for (const std::size_t expected : std::array<std::size_t, 2>{3, 2}) {
        const auto frame = pv::NextPresentableFrame(navigation, ready);
        Check(frame && *frame == expected,
              "direction change must discard repeated work without reordering commitments");
        navigation.CompletePresentation(*frame);
    }
    Check(navigation.Empty(),
          "stale completion for a cancelled repeated frame must remain unauthorized");

    bool rejected_nonmonotonic = false;
    try {
        pv::UploadLedger invalid(2);
        invalid.Queue(pv::UploadTicket{0, 1, 5});
        invalid.Queue(pv::UploadTicket{1, 1, 4});
    } catch (const std::logic_error&) {
        rejected_nonmonotonic = true;
    }
    Check(rejected_nonmonotonic,
          "upload ledger must make the D3D monotonic fence premise explicit");
}

}  // namespace

int main() {
    ConfigDefaultTests();
    const std::size_t physical_core_count = ProcessorTopologyTests();
    NavigationTests();
    ReservationPlannerTests();
    PngTests();
    ResourceSlotTests();
    ReservationTests();
    CompletionQueueTests();
    RuntimeTelemetryTests();
    ReservationByteBudgetTests();
    ControlledCompletionOrderingTests();
    std::cout << "PASS: core tests physical_core_count=" << physical_core_count
              << " default_worker_count=" << pv::DefaultWorkerCount() << '\n';
    return 0;
}
