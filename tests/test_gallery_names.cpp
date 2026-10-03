#include "rf/gallery/Gallery.h"

#include "test_framework.h"

TEST(Gallery_ReadsTheAppFromTheFileName) {
    CHECK(rf::AppFromFileName("Reframe_Discord_2026-10-02_00-32-05.mp4") == "Discord");
    CHECK(rf::AppFromFileName("Reframe_Reframe Demo Game_2026-10-02_00-06-10_kill+triple.mp4") ==
          "Reframe Demo Game");
}

TEST(Gallery_TreatsGenericClipsAsTheDesktop) {
    CHECK(rf::AppFromFileName("Reframe_Replay_2026-09-30_20-02-39.mp4").empty());
    CHECK(rf::AppFromFileName("Reframe_Recording_2026-09-30_20-02-39.mp4").empty());
    CHECK(rf::AppFromFileName("my clip.mp4").empty());
}
