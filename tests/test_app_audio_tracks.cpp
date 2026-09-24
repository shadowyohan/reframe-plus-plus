#include "rf/audio/AppAudioTracks.h"
#include "rf/audio/MaxineSetup.h"

#include "test_framework.h"

using namespace rf;

TEST(AppAudioTracks_AppKeepsTheSlotItAlreadyHas) {
    CHECK_EQ(AppAudioTracks::PickSlot({100, 200, 0}, 200, 3), 1u);
}

TEST(AppAudioTracks_NewAppTakesTheFirstFreeSlot) {
    CHECK_EQ(AppAudioTracks::PickSlot({100, 0, 0}, 300, 3), 1u);
    CHECK_EQ(AppAudioTracks::PickSlot({}, 300, 6), 0u);
}

TEST(AppAudioTracks_FullSlotsReportOverflow) {
    CHECK_EQ(AppAudioTracks::PickSlot({1, 2, 3}, 4, 3), 3u);
}

TEST(MaxineSetup_ComputeCapabilityPicksTheInstaller) {
    CHECK(RtxGenerationFromComputeCapability(6, 1) == RtxGeneration::Unsupported);
    CHECK(RtxGenerationFromComputeCapability(7, 0) == RtxGeneration::Unsupported);
    CHECK(RtxGenerationFromComputeCapability(7, 5) == RtxGeneration::Turing);
    CHECK(RtxGenerationFromComputeCapability(8, 6) == RtxGeneration::Ampere);
    CHECK(RtxGenerationFromComputeCapability(8, 9) == RtxGeneration::Ada);
    CHECK(RtxGenerationFromComputeCapability(12, 0) == RtxGeneration::Blackwell);
    CHECK(MaxineInstallerUrl(RtxGeneration::Unsupported) == nullptr);
    CHECK(MaxineInstallerUrl(RtxGeneration::Ada) != nullptr);
}
