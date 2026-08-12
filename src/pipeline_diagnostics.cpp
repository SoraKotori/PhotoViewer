#include "pipeline_runtime.h"

#include <array>
#include <ostream>

namespace pv {
namespace {

std::size_t RetiringCount(const ReservationTable& table) noexcept {
    std::size_t count = 0;
    for (ReservationId id = 0; id < table.Capacity(); ++id) {
        if (table.At(id).retiring) ++count;
    }
    return count;
}

}  // namespace

void PipelineRuntime::WriteDiagnostics(std::ostream& output) const {
    constexpr std::array stage_names{
        "Outside", "WaitingIo", "IoInFlight", "CompressedReady",
        "DecodeQueued", "DecodedStagingAvailable", "Uploading",
        "PresentationTextureAvailable", "CancelPending", "Failed"};
    std::array<std::size_t, stage_names.size()> stage_counts{};
    for (std::size_t index = 0; index < model_.FrameCount(); ++index) {
        const auto stage = static_cast<std::size_t>(scheduler_.StageOf(index));
        if (stage < stage_counts.size()) ++stage_counts[stage];
    }

    output << "iocp_enabled=" << (storage_.Enabled() ? 1 : 0) << '\n'
           << "io_prefix_granularity="
           << storage_.TransferGranularity() << '\n';
    for (std::size_t index = 0; index < stage_names.size(); ++index) {
        output << stage_names[index] << '=' << stage_counts[index] << '\n';
    }

    const auto write_stage_indices = [&](const std::string_view name,
                                         const PipelineStage stage) {
        output << name << '=';
        bool first = true;
        for (std::size_t index = 0; index < model_.Frames().size(); ++index) {
            if (scheduler_.StageOf(index) != stage) continue;
            if (!first) output << ',';
            output << index;
            first = false;
        }
        output << '\n';
    };
    write_stage_indices("DecodedStagingAvailable_indices",
                        PipelineStage::DecodedStagingAvailable);
    write_stage_indices("Uploading_indices", PipelineStage::Uploading);
    write_stage_indices("PresentationTextureAvailable_indices",
                        PipelineStage::PresentationTextureAvailable);

    output << "ActiveReadableGpuTexture_indices=";
    bool first_readable = true;
    for (std::size_t index = 0; index < model_.Frames().size(); ++index) {
        if (!graphics_.HasReadableTexture(index)) continue;
        if (!first_readable) output << ',';
        output << index;
        first_readable = false;
    }
    output << '\n'
           << "compressed_committed_bytes="
           << resources_.SlotsView().CompressedCommittedBytes() << '\n'
           << "staging_committed_bytes="
           << resources_.Slots().StagingCommittedBytes() << '\n'
           << "gpu_bytes=" << graphics_.GpuBytes() << '\n'
           << "free_compressed_slots=" << resources_.Slots().FreeCompressedCount()
           << '\n'
           << "free_staging_slots=" << resources_.Slots().FreeStagingCount() << '\n'
           << "inactive_gpu_texture_slots="
           << resources_.Slots().InactiveGpuTextureCount() << '\n'
           << "compressed_reservations="
           << model_.Reservations().Compressed().AssignedCount() << '/'
           << model_.Reservations().Compressed().Capacity() << '\n'
           << "staging_reservations="
           << model_.Reservations().Staging().AssignedCount() << '/'
           << model_.Reservations().Staging().Capacity() << '\n'
           << "gpu_texture_reservations="
           << model_.Reservations().GpuTextures().AssignedCount() << '/'
           << model_.Reservations().GpuTextures().Capacity() << '\n'
           << "compressed_retiring_reservations="
           << RetiringCount(model_.Reservations().Compressed()) << '\n'
           << "staging_retiring_reservations="
           << RetiringCount(model_.Reservations().Staging()) << '\n'
           << "gpu_texture_retiring_reservations="
           << RetiringCount(model_.Reservations().GpuTextures()) << '\n'
           << "retiring_reservations="
           << RetiringCount(model_.Reservations().Compressed()) +
                  RetiringCount(model_.Reservations().Staging()) +
                  RetiringCount(model_.Reservations().GpuTextures())
           << '\n'
           << "work_queue=" << decode_.QueuedWorkCount() << '\n'
           << "uploads=" << graphics_.UploadCount() << '\n'
           << "held_direction=" << model_.NavigationView().HeldDirection() << '\n'
           << "current_index=" << model_.NavigationView().CurrentIndex() << '\n'
           << "title_matches_current=";

    std::array<wchar_t, 512> window_title{};
    GetWindowTextW(window_.Handle(), window_title.data(),
                   static_cast<int>(window_title.size()));
    const std::filesystem::path current_filename =
        model_.CatalogItemAt(model_.NavigationView().CurrentIndex()).path.filename();
    output << (current_filename.native() == window_title.data() ? 1 : 0) << '\n'
           << "next_index=";
    if (const auto next = model_.NavigationView().NextIndex()) {
        output << *next;
    } else {
        output << "none";
    }
    output << '\n'
           << "decode_count=" << decode_.DecodeCount()
           << '\n'
           << "decode_nanoseconds="
           << decode_.DecodeNanoseconds() << '\n'
           << "upload_count=" << graphics_.SubmittedUploadCount() << '\n'
           << "upload_nanoseconds=" << graphics_.UploadNanoseconds() << '\n'
           << "draw_count=" << graphics_.DrawCount() << '\n'
           << "draw_nanoseconds=" << graphics_.DrawNanoseconds() << '\n';
}

}  // namespace pv
