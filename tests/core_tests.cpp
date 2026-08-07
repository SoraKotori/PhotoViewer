#include "navigation.h"
#include "png.h"
#include "processor_topology.h"
#include "resource_slots.h"
#include "reservation.h"
#include "work_queue.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::size_t ProcessorTopologyTests() {
    Check(pv::DefaultWorkerCountForPhysicalCores(0) == 1,
          "missing topology must retain one worker");
    Check(pv::DefaultWorkerCountForPhysicalCores(1) == 1,
          "one physical core must select one worker");
    Check(pv::DefaultWorkerCountForPhysicalCores(8) == 8,
          "worker default must follow physical cores below the cap");
    Check(pv::DefaultWorkerCountForPhysicalCores(16) == 16,
          "worker default must include the cap");
    Check(pv::DefaultWorkerCountForPhysicalCores(32) == 16,
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
    navigation.Step(1, true, 4);
    for (const std::size_t index : std::array<std::size_t, 5>{6, 7, 8, 9, 10}) {
        Check(PresentNext(navigation) == index,
              "merged key repeat count must preserve every adjacent image");
    }
    Check(navigation.Empty(), "merged key repeat count must drain exactly");

    navigation.Reset(5, 20);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true, 4);
    navigation.Release(1);
    Check(PresentNext(navigation) == 6,
          "release must discard every unpresented merged repeat");
    Check(navigation.Empty(), "merged repeats must not survive release");

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
    const auto left_plan = navigation.PlannedOrder(4);
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

void PngTests() {
    std::array<std::byte, 33> header{};
    const std::array<unsigned char, 24> prefix{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x10, 0xE0};
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        header[index] = std::byte{prefix[index]};
    }
    const auto parsed = pv::ParsePngHeader(header);
    Check(parsed.has_value(), "valid PNG IHDR");
    Check(parsed->width == 7680 && parsed->height == 4320, "8K dimensions");
    Check(parsed->decoded_bytes == 7680ULL * 4320ULL * 4ULL,
          "decoded byte reservation");
    header[0] = std::byte{0};
    Check(!pv::ParsePngHeader(header), "invalid signature must fail");
}

void ResourceSlotTests() {
    pv::ResourceSlots slots(2, 2, 2, 8192, 8192);
    Check(slots.CompressedCount() == 2 && slots.FreeCompressedCount() == 2,
          "compressed slot storage and free index must start at configured count");
    Check(slots.StagingCount() == 2 && slots.FreeStagingCount() == 2,
          "staging slot storage and free index must start at configured count");
    Check(slots.GpuTextureCount() == 2 && slots.FreeGpuTextureCount() == 2,
          "GPU slot storage and free index must start at configured count");

    const pv::SlotId compressed0 = slots.AcquireCompressed(4096, 3, 7);
    const pv::SlotId compressed1 = slots.AcquireCompressed(4096, 4, 7);
    Check(compressed0 != pv::kInvalidSlot && compressed1 != pv::kInvalidSlot,
          "configured compressed slots must be allocatable");
    Check(slots.FreeCompressedCount() == 0,
          "compressed free index must exclude occupied slots");
    Check(slots.AcquireCompressed(1, 5, 7) == pv::kInvalidSlot,
          "compressed slot count must be a hard limit");
    Check(slots.Compressed(compressed0).state ==
              pv::CompressedSlotState::FileReadDestination,
          "compressed acquisition state must describe its pipeline phase");
    pv::IoRequest* const io_address = &slots.Compressed(compressed0).io;
    slots.Compressed(compressed0).state =
        pv::CompressedSlotState::CompressedDataAvailable;
    slots.ReleaseCompressed(compressed0);
    Check(slots.FreeCompressedCount() == 1 &&
              slots.Compressed(compressed0).state == pv::CompressedSlotState::Free,
          "compressed release must restore state and free index together");
    const pv::SlotId recycled_compressed = slots.AcquireCompressed(4096, 5, 8);
    Check(recycled_compressed == compressed0 &&
              &slots.Compressed(recycled_compressed).io == io_address,
          "compressed slot must retain an inline stable I/O request");
    slots.ReleaseCompressed(recycled_compressed);

    const pv::SlotId staging0 = slots.AcquireStaging(4096, 3, 7);
    const pv::SlotId staging1 = slots.AcquireStaging(4096, 4, 7);
    Check(staging0 != pv::kInvalidSlot && staging1 != pv::kInvalidSlot,
          "configured staging slots must be allocatable");
    Check(slots.AcquireStaging(1, 5, 7) == pv::kInvalidSlot,
          "staging slot count must be a hard limit");
    Check(slots.StagingAt(staging0).state ==
              pv::StagingSlotState::Prepared,
          "staging acquisition must reserve an unmapped prepared slot");
    pv::WorkToken* const token_address = &slots.WorkTokenAt(staging0);
    slots.StagingAt(staging0).state =
        pv::StagingSlotState::DecodedPixelsAvailable;
    slots.ReleaseStaging(staging0);
    Check(slots.FreeStagingCount() == 1 &&
              slots.StagingAt(staging0).state == pv::StagingSlotState::Free,
          "staging release must restore state and free index together");
    const pv::SlotId recycled_staging = slots.AcquireStaging(4096, 5, 8);
    Check(recycled_staging == staging0 &&
              &slots.WorkTokenAt(recycled_staging) == token_address,
          "staging slot must retain a stable preallocated work token");
    slots.ReleaseStaging(recycled_staging);

    constexpr pv::SlotId gpu0 = 0;
    constexpr pv::SlotId gpu1 = 1;
    Check(slots.ActivateGpuTexture(gpu0) && slots.ActivateGpuTexture(gpu1),
          "configured SourceTexture slots must be activatable once");
    Check(!slots.ActivateGpuTexture(2),
          "SourceTexture slot count must be a hard limit");
    Check(slots.GpuTextureAt(gpu0).state ==
              pv::GpuTextureSlotState::Writable,
          "GPU acquisition state must describe its pipeline phase");
    Check(slots.FreeGpuTextureCount() == 0,
          "fixed SourceTexture slots leave the inactive index when activated");

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
    reconcile({12, 11}, true);
    Check(table.FindFrame(12) != pv::kInvalidReservation &&
              table.FindFrame(10) == pv::kInvalidReservation,
          "safe retired reservation must be reassigned without a second pool");

    pv::WorkQueue queue;
    pv::WorkToken token;
    pv::DecodeWork work{3, 7, &token, 1, 2};
    Check(queue.TryPush(work), "decode work must enter queue");
    pv::DecodeWork cancelled;
    Check(queue.TryCancel(&token, cancelled) && cancelled.index == 3,
          "unclaimed decode work must be synchronously cancellable");
    Check(queue.Size() == 0 &&
              token.claim.load(std::memory_order_acquire) == pv::WorkClaim::Cancelled,
          "cancelled work must leave the queue exactly once");

    pv::WorkToken low_token;
    pv::WorkToken high_token;
    pv::DecodeWork low{9, 1, &low_token, 0, 0};
    pv::DecodeWork high{4, 1, &high_token, 0, 0};
    Check(queue.TryPush(low) && queue.TryPush(high),
          "priority reorder test work must enter queue");
    queue.Reorder(std::array<std::size_t, 2>{4, 9});
    pv::DecodeWork popped;
    Check(queue.Pop(popped, std::stop_token{}) && popped.index == 4,
          "queued decode work must follow the latest navigation priority");

    pv::WorkQueue naturally_bounded_queue;
    std::array<pv::WorkToken, 32> natural_tokens;
    for (std::size_t index = 0; index < 32; ++index) {
        pv::DecodeWork queued{index, 1, &natural_tokens[index], 0, 0};
        Check(naturally_bounded_queue.TryPush(queued),
              "work queue must not impose an independent item-count limit");
    }
    Check(naturally_bounded_queue.Size() == 32,
          "work queue size must be bounded only by dispatched slot-backed work");
}

void CompletionQueueTests() {
    pv::CompletionQueue queue;
    Check(queue.PushReleasedInput(pv::ReleasedInput{3, 7, 1}),
          "first completion event must request notification");
    Check(!queue.Push(pv::DecodeResult{3, 7, true, false, S_OK, 2}),
          "pending completion events must coalesce notification");

    pv::CompletionQueue::Batch batch = queue.DrainAll();
    Check(batch.released_inputs.size() == 1 && batch.results.size() == 1,
          "one critical section must drain both completion event classes");
    Check(queue.Push(pv::DecodeResult{4, 7, true, false, S_OK, 3}),
          "atomic drain and acknowledgement must re-arm notification");
    batch = queue.DrainAll();
    Check(batch.results.size() == 1 && batch.results.front().index == 4,
          "completion queue must remain reusable after a batch drain");
}

}  // namespace

int main() {
    const std::size_t physical_core_count = ProcessorTopologyTests();
    NavigationTests();
    PngTests();
    ResourceSlotTests();
    ReservationTests();
    CompletionQueueTests();
    std::cout << "PASS: core tests physical_core_count=" << physical_core_count
              << " default_worker_count=" << pv::DefaultWorkerCount() << '\n';
    return 0;
}
