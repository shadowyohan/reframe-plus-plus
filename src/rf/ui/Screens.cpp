#include "rf/ui/Screens.h"

#include "rf/core/Lang.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

namespace rf::ui {
namespace {

using namespace theme;

struct Column {
    float x = 0, y = 0, w = 0;

    float* bottom = nullptr;

    ImVec2 Next(float height) {
        const ImVec2 pos(x, y);
        y += height + kItemGap;
        if (bottom) *bottom = y;
        return pos;
    }
    void Skip(float height) {
        y += height + kItemGap;
        if (bottom) *bottom = y;
    }

    [[nodiscard]] ImVec2 Peek() const { return ImVec2(x, y); }
};

constexpr float kH1Height = 36.0f;
constexpr float kH2Height = 29.0f;

std::string FormatHms(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string FormatGb(std::uint64_t bytes) {
    return TrFormat("{:.1f} ГБ", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
}

std::string ElideEnd(UiContext& ctx, Font font, const std::string& utf8, float max_w) {
    if (MeasureText(ctx, font, utf8.c_str()).x <= max_w) return utf8;
    const std::wstring wide = ToWide(utf8);
    for (std::size_t len = wide.size(); len > 4; --len) {
        std::string candidate = ToUtf8(wide.substr(0, len)) + "...";
        if (MeasureText(ctx, font, candidate.c_str()).x <= max_w) return candidate;
    }
    return "...";
}

bool DiscButton(UiContext& ctx, std::uint32_t id, ImVec2 pos, float size, const char* icon,
                float icon_size, ImU32 disc_col = kCircleBtn, ImU32 icon_col = kText,
                bool honour_reserved = true) {
    Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + size, pos.y + size), honour_reserved);
    const float scale = 1.0f + 0.10f * it.hover - 0.12f * it.press;
    const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);

    ctx.dl->AddCircleFilled(ctx.At(center), size * 0.5f * scale, ctx.Fade(disc_col), 48);
    DrawIcon(ctx, icon, ImVec2(center.x - icon_size * 0.5f, center.y - icon_size * 0.5f),
             icon_size, icon_col, scale);
    return it.clicked;
}

void Footer(UiContext& ctx, ImVec2 panel_min, ImVec2 panel_max, const AppModel& model) {
    DrawText(ctx, Font::Tiny, ImVec2(panel_min.x + kPanelPad, panel_max.y - kPanelPad - 15.0f),
             kTextFaint, model.build_line.c_str());
}

ImVec2 BackRowPos(ImVec2 panel_min, ImVec2 panel_max) {
    return ImVec2(panel_min.x + kPanelPad,
                  panel_max.y - kPanelPad - 15.0f - kItemGap - kRowH_TwoLine);
}

void ReserveBackRow(UiContext& ctx, ImVec2 panel_min, ImVec2 panel_max, float content_w) {
    const ImVec2 pos = BackRowPos(panel_min, panel_max);
    ctx.Reserve(pos, ImVec2(pos.x + content_w, panel_max.y));
}

bool BackRow(UiContext& ctx, ImVec2 panel_min, ImVec2 panel_max, float content_w) {
    const ImVec2 pos = BackRowPos(panel_min, panel_max);
    Interaction it = Row(ctx, HashId("back-row"), pos, ImVec2(content_w, kRowH_TwoLine), true,
                         false);
    DrawText(ctx, Font::Body, ImVec2(pos.x + 15.0f, pos.y + 23.0f), kText, "Назад");

    const bool clicked =
        CircleButton(ctx, HashId("back-btn"), ImVec2(pos.x + content_w - 62.0f, pos.y + 10.0f),
                     kCircleBtnSize, "arrow-circle", true,
                     false);
    return it.clicked || clicked;
}

}

std::string DescribeHotkey(std::uint32_t mods, std::uint32_t vk) {
    if (vk == 0) return "—";

    std::string out;
    if (mods & 0x0002) out += "CTRL+";
    if (mods & 0x0001) out += "ALT+";
    if (mods & 0x0004) out += "SHIFT+";

    if (vk >= 0x70 && vk <= 0x87)
        out += std::format("F{}", vk - 0x70 + 1);
    else if (vk >= 'A' && vk <= 'Z')
        out += static_cast<char>(vk);
    else
        out += std::format("0x{:02X}", vk);
    return out;
}

void Menu::Open() {
    open_ = true;
    slide_.SetTarget(0.0f);
}

void Menu::Close() {
    open_ = false;

    ClosePlayer();

    slide_.SetTarget(-600.0f);
}

void Menu::ToggleOpen() {
    if (open_)
        Close();
    else
        Open();
}

bool Menu::visible() const { return open_ || slide_.value() > -595.0f; }

void Menu::Navigate(Page page, bool forward) {
    if (page == page_) return;
    previous_page_ = page_;
    page_ = page;
    forward_ = forward;
    transitioning_ = true;
    mic_list_open_ = false;
    gpu_list_open_ = false;
    monitor_list_open_ = false;
    transition_.Reset(0.0f);
    transition_.SetTarget(1.0f);
}

void Menu::Draw(UiContext& ctx, ImVec2 screen, AppModel& model, TextureCache& textures) {
    const float x = slide_.Update(ctx.dt, open_ ? spring::kMenu : spring::kExit);
    if (!open_ && x <= -595.0f) {

        page_ = Page::Main;
        previous_page_ = Page::Main;
        transitioning_ = false;
        mic_list_open_ = false;
        gpu_list_open_ = false;
        monitor_list_open_ = false;
        capture_row_ = -1;
        rejected_row_ = -1;
        return;
    }

    const float openness = std::clamp(1.0f + x / 700.0f, 0.0f, 1.0f);
    ctx.dl->AddRectFilled(ImVec2(0, 0), screen,
                          IM_COL32(0, 0, 0, static_cast<int>(102 * openness)));

    ctx.offset = ImVec2(std::round(x), 0.0f);

    const float col_x = kScreenPad;
    const float header_y = kScreenPad;

    ctx.dl->AddRectFilled(ctx.At(ImVec2(col_x, header_y)),
                          ctx.At(ImVec2(col_x + kLogoBox, header_y + kLogoBox)),
                          ctx.Fade(kPanelBg), kLogoRadius);
    ctx.dl->AddRect(ctx.At(ImVec2(col_x, header_y)),
                    ctx.At(ImVec2(col_x + kLogoBox, header_y + kLogoBox)), ctx.Fade(kPanelBorder),
                    kLogoRadius, 0, 1.0f);

    static float spin = 0.0f;
    if (model.recording) spin += ctx.dt * 1.6f;
    DrawIcon(ctx, "logo-mark", ImVec2(col_x + 12.0f, header_y + 13.0f), 34.0f, kText, 1.0f, spin);

    const float notch_x = col_x + kLogoBox;
    DrawIcon(ctx, "logo-notch", ImVec2(notch_x, header_y + (kLogoBox - kNotchH) * 0.5f), kNotchH,
             kNotchTint);

    const ImVec2 word = MeasureText(ctx, Font::H1, "reframe++");
    const float pill_x = notch_x + kNotchW;
    const float pill_w = word.x + 20.0f;
    const float pill_h = word.y + 20.0f;
    const float pill_y = header_y + (kLogoBox - pill_h) * 0.5f;
    ctx.dl->AddRectFilled(ctx.At(ImVec2(pill_x, pill_y)),
                          ctx.At(ImVec2(pill_x + pill_w, pill_y + pill_h)), ctx.Fade(kPanelBg),
                          kTitlePillRadius);
    ctx.dl->AddRect(ctx.At(ImVec2(pill_x, pill_y)),
                    ctx.At(ImVec2(pill_x + pill_w, pill_y + pill_h)), ctx.Fade(kPanelBorder),
                    kTitlePillRadius, 0, 1.0f);
    DrawText(ctx, Font::H1, ImVec2(pill_x + 10.0f, pill_y + 10.0f), kText, "reframe++");

    const ImVec2 panel_min(col_x, header_y + kLogoBox + kHeaderGap);
    const ImVec2 panel_max(col_x + kPanelW, screen.y - kScreenPad);
    DrawPanel(ctx, panel_min, panel_max, kPanelRadius);

    ctx.dl->PushClipRect(ctx.At(ImVec2(panel_min.x + 1, panel_min.y + 1)),
                         ctx.At(ImVec2(panel_max.x - 1, panel_max.y - 1)), true);

    if (transitioning_) {
        const float t = transition_.Update(ctx.dt, spring::kExit);
        if (transition_.settled()) transitioning_ = false;

        const ImVec2 base = ctx.offset;
        const float alpha = ctx.alpha;

        ctx.offset =
            ImVec2(base.x + std::round(forward_ ? -45.0f * t : 501.0f * t), base.y);
        ctx.alpha = alpha * std::clamp(1.0f - t, 0.0f, 1.0f);

        const bool was_interactive = ctx.interactive;
        ctx.interactive = false;
        DrawPage(ctx, previous_page_, panel_min, panel_max, model, textures);
        ctx.interactive = was_interactive;

        ctx.offset = ImVec2(
            base.x + std::round(forward_ ? 501.0f * (1.0f - t) : -45.0f * (1.0f - t)), base.y);
        ctx.alpha = alpha * std::clamp(t * 1.4f, 0.0f, 1.0f);
        DrawPage(ctx, page_, panel_min, panel_max, model, textures);

        ctx.offset = base;
        ctx.alpha = alpha;
    } else {
        DrawPage(ctx, page_, panel_min, panel_max, model, textures);
    }

    ctx.dl->PopClipRect();
    ctx.offset = ImVec2(0, 0);

    PlayerPanel(ctx, screen, model);
}

void Menu::OpenInPlayer(AppModel& model, const std::filesystem::path& file) {

    if (!model.player) {
        if (model.on_open_file) model.on_open_file(file);
        return;
    }
    if (auto s = model.player->Open(file); !s) {
        RF_WARN("player: {}", s.str());
        if (model.on_open_file) model.on_open_file(file);
        return;
    }
    player_ = model.player;
    player_->SetVolume(volume_);
    player_->Play();
    player_open_ = true;
    player_slide_.Reset(60.0f);
    player_slide_.SetTarget(0.0f);
}

void Menu::ClosePlayer(AppModel& model) {
    player_ = model.player;
    ClosePlayer();
}

void Menu::ClosePlayer() {
    if (player_) player_->Close();
    player_open_ = false;
}

void Menu::PlayerPanel(UiContext& ctx, ImVec2 screen, AppModel& model) {
    if (!player_open_ || !model.player) return;

    VideoPlayer& player = *model.player;
    player.Update();

    const float left = kScreenPad + kPanelW + 10.0f;
    const ImVec2 panel_min(left, kScreenPad);
    const ImVec2 panel_max(screen.x - kScreenPad, screen.y - kScreenPad);
    if (panel_max.x - panel_min.x < 480.0f) return;

    const float slide = player_slide_.Update(ctx.dt, spring::kMenu);
    ctx.offset = ImVec2(0.0f, slide);
    ctx.alpha = std::clamp(1.0f - slide / 60.0f, 0.0f, 1.0f);

    DrawPanel(ctx, panel_min, panel_max, kPanelRadius);

    constexpr float kPad = 20.0f, kGap = 30.0f;
    const float content_x = panel_min.x + kPad;
    const float content_w = (panel_max.x - panel_min.x) - 2 * kPad;
    float y = panel_min.y + kPad;

    {

        const std::string stem = player.file().stem().string();
        std::string source = "Запись";
        if (const auto first = stem.find('_'); first != std::string::npos) {
            const auto second = stem.find('_', first + 1);
            if (second != std::string::npos && second > first + 1)
                source = stem.substr(first + 1, second - first - 1);
        }

        DrawText(ctx, Font::H1, ImVec2(content_x, y), kAppAccent, source.c_str());
        const std::string name = player.file().filename().string();
        DrawText(ctx, Font::Body, ImVec2(content_x, y + 46.0f), kText, name.c_str());

        const ImVec2 close_pos(panel_max.x - kPad - 50.0f, y);
        Interaction close = Hit(ctx, HashId("player-close"), close_pos,
                                ImVec2(close_pos.x + 50.0f, close_pos.y + 50.0f));
        DrawIcon(ctx, "close", close_pos, 50.0f, kText, 1.0f + 0.12f * close.hover - 0.1f * close.press);
        if (close.clicked) {
            ClosePlayer(model);
            ctx.offset = ImVec2(0, 0);
            ctx.alpha = 1.0f;
            return;
        }
        y += 70.0f + kGap;
    }

    const float video_h = content_w * 9.0f / 16.0f;
    {
        const ImVec2 vmin(content_x, y);
        const ImVec2 vmax(content_x + content_w, y + video_h);
        ctx.dl->AddRectFilled(ctx.At(vmin), ctx.At(vmax), ctx.Fade(IM_COL32(0, 0, 0, 255)), 23.0f);

        if (ImTextureID tex = reinterpret_cast<ImTextureID>(player.frame())) {

            const float src_aspect = static_cast<float>(player.width()) /
                                     std::max(1u, player.height());
            float w = content_w, h = content_w / src_aspect;
            if (h > video_h) { h = video_h; w = video_h * src_aspect; }
            const ImVec2 c((vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f);
            ctx.dl->AddImageRounded(tex, ctx.At(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f)),
                                    ctx.At(ImVec2(c.x + w * 0.5f, c.y + h * 0.5f)), ImVec2(0, 0),
                                    ImVec2(1, 1), ctx.Fade(kText), 23.0f);
        }

        Interaction it = Hit(ctx, HashId("player-video"), vmin, vmax);
        if (it.clicked) player.TogglePlay();
        y += video_h + kGap;
    }

    {
        constexpr float kBtn = 40.0f, kCtlGap = 20.0f;
        const float row_y = y;
        const float center_y = row_y + kBtn * 0.5f;
        float x = content_x;

        Interaction play = Hit(ctx, HashId("player-play"), ImVec2(x, row_y),
                               ImVec2(x + kBtn, row_y + kBtn));
        const float scale = 1.0f + 0.12f * play.hover - 0.1f * play.press;
        if (player.playing()) {

            const ImVec2 c(x + kBtn * 0.5f, center_y);
            ctx.dl->AddCircleFilled(c, kBtn * 0.5f * scale, ctx.Fade(kText), 32);
            const float bw = 3.5f * scale, bh = 9.0f * scale, bx = 4.0f * scale;
            ctx.dl->AddRectFilled(ImVec2(c.x - bx - bw, c.y - bh), ImVec2(c.x - bx + bw, c.y + bh),
                                  ctx.Fade(kPanelBg), 1.5f);
            ctx.dl->AddRectFilled(ImVec2(c.x + bx - bw, c.y - bh), ImVec2(c.x + bx + bw, c.y + bh),
                                  ctx.Fade(kPanelBg), 1.5f);
        } else {
            DrawIcon(ctx, "play", ImVec2(x, row_y), kBtn, kText, scale);
        }
        if (play.clicked) player.TogglePlay();
        x += kBtn + kCtlGap;

        const std::string time = std::format("{:02}:{:02} / {:02}:{:02}",
                                             static_cast<int>(player.position()) / 60,
                                             static_cast<int>(player.position()) % 60,
                                             static_cast<int>(player.duration()) / 60,
                                             static_cast<int>(player.duration()) % 60);
        const float time_w = MeasureText(ctx, Font::Body, time.c_str()).x;
        constexpr float kVolSlider = 155.0f;
        const float right_w = time_w + kCtlGap + kBtn + 10.0f + kVolSlider + 40.0f + 2 * kBtn +
                              10.0f;
        const float seek_w = std::max(80.0f, content_x + content_w - x - kCtlGap - right_w);

        {
            float pos = static_cast<float>(player.position());
            const float total = static_cast<float>(std::max(0.001, player.duration()));
            if (SliderBar(ctx, HashId("player-seek"), ImVec2(x, center_y - 13.0f), seek_w, 26.0f,
                          pos, 0.0f, total))
                player.Seek(pos);
            x += seek_w + kCtlGap;
        }

        DrawText(ctx, Font::Body, ImVec2(x, center_y - 12.0f), kText, time.c_str());
        x += time_w + kCtlGap;

        {
            Interaction mute = Hit(ctx, HashId("player-mute"), ImVec2(x, row_y),
                                   ImVec2(x + kBtn, row_y + kBtn));

            DrawIcon(ctx, volume_ > 0.001f ? "volume-high" : "volume-off", ImVec2(x, row_y), kBtn,
                     kText,
                     1.0f + 0.1f * mute.hover);
            if (mute.clicked) {
                volume_ = volume_ > 0.001f ? 0.0f : 1.0f;
                player.SetVolume(volume_);
            }
            x += kBtn + 10.0f;

            float v = volume_ * 100.0f;
            if (SliderBar(ctx, HashId("player-volume"), ImVec2(x, center_y - 11.0f), kVolSlider,
                          22.0f, v, 0.0f, 100.0f)) {
                volume_ = v / 100.0f;
                player.SetVolume(volume_);
            }
        }

        {
            float rx = content_x + content_w - 2 * kBtn - 10.0f;
            Interaction folder = Hit(ctx, HashId("player-folder"), ImVec2(rx, row_y),
                                     ImVec2(rx + kBtn, row_y + kBtn));
            DrawIcon(ctx, "folder", ImVec2(rx, row_y), kBtn, kText, 1.0f + 0.1f * folder.hover);
            if (folder.clicked && model.on_reveal_file) model.on_reveal_file(player.file());
            rx += kBtn + 10.0f;

            Interaction trash = Hit(ctx, HashId("player-trash"), ImVec2(rx, row_y),
                                    ImVec2(rx + kBtn, row_y + kBtn));
            DrawIcon(ctx, "trash", ImVec2(rx, row_y), kBtn, kWarnRed, 1.0f + 0.1f * trash.hover);
            if (trash.clicked && model.on_delete_file) {

                const auto doomed = player.file();
                ClosePlayer(model);
                model.on_delete_file(doomed);
                ctx.offset = ImVec2(0, 0);
                ctx.alpha = 1.0f;
                return;
            }
        }
    }

    ctx.offset = ImVec2(0, 0);
    ctx.alpha = 1.0f;
}

void Menu::DrawPage(UiContext& ctx, Page page, ImVec2 panel_min, ImVec2 panel_max, AppModel& model,
                    TextureCache& textures) {

    ctx.reserved.clear();

    const int index = static_cast<int>(page);
    const ImVec2 view_min = panel_min;
    const ImVec2 view_max(panel_max.x, BackRowPos(panel_min, panel_max).y - kItemGap);

    const float measured = page_height_[index];
    const bool scrolls = page != Page::Gallery && measured > view_max.y - view_min.y;
    if (scrolls)
        page_scroll_.Begin(ctx, HashId("page-scroll", static_cast<std::uint32_t>(index)), view_min,
                           view_max, measured);

    content_bottom_ = panel_min.y + kPanelPad;

    switch (page) {
        case Page::Main:     PageMain(ctx, panel_min, panel_max, model, textures); break;
        case Page::Settings: PageSettings(ctx, panel_min, panel_max, model); break;
        case Page::Video:    PageVideo(ctx, panel_min, panel_max, model); break;
        case Page::Audio:    PageAudio(ctx, panel_min, panel_max, model); break;
        case Page::Disk:     PageDisk(ctx, panel_min, panel_max, model); break;
        case Page::Interface: PageInterface(ctx, panel_min, panel_max, model); break;
        case Page::Keybinds: PageKeybinds(ctx, panel_min, panel_max, model); break;
        case Page::Gallery:  PageGallery(ctx, panel_min, panel_max, model, textures); break;
    }

    page_height_[index] = content_bottom_ - view_min.y + kPanelPad;
    if (scrolls) page_scroll_.End(ctx);

    if (page != Page::Main) {
        const float content_w = kPanelW - 2 * kPanelPad;
        if (BackRow(ctx, panel_min, panel_max, content_w)) {
            capture_row_ = -1;
            Navigate(page == Page::Settings || page == Page::Gallery ? Page::Main : Page::Settings,
                     false);
        }
    }
    Footer(ctx, panel_min, panel_max, model);
}

void Menu::PageMain(UiContext& ctx, ImVec2 panel_min, ImVec2 panel_max, AppModel& model,
                    TextureCache& textures) {
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "галерея");

    {
        const ImVec2 pos = col.Next(kGalleryStripH);
        ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(ImVec2(pos.x + col.w, pos.y + kGalleryStripH)),
                              ctx.Fade(kRowBg), kRowRadius);

        const auto& items = model.gallery_items;
        const float tile = kGalleryStripH - 20.0f;
        float tx = pos.x + 10.0f;

        const ImVec2 open_pos(pos.x + col.w - 54.0f, pos.y + kGalleryStripH * 0.5f - 22.0f);
        ctx.Reserve(open_pos, ImVec2(open_pos.x + 44.0f, open_pos.y + 44.0f));

        ctx.dl->PushClipRect(ctx.At(pos), ctx.At(ImVec2(pos.x + col.w, pos.y + kGalleryStripH)),
                             true);

        for (std::size_t i = 0; i < items.size() && i < 4; ++i) {
            const auto& item = items[i];
            const ImVec2 tile_pos(tx, pos.y + 10.0f);
            const std::uint32_t id = HashId("strip", static_cast<std::uint32_t>(i));
            Interaction it = Hit(ctx, id, tile_pos, ImVec2(tile_pos.x + tile, tile_pos.y + tile));

            const float scale = 1.0f + 0.06f * it.hover - 0.04f * it.press;
            const ImVec2 center(tile_pos.x + tile * 0.5f, tile_pos.y + tile * 0.5f);
            const ImVec2 half(tile * 0.5f * scale, tile * 0.5f * scale);
            const ImVec2 a = ctx.At(ImVec2(center.x - half.x, center.y - half.y));
            const ImVec2 b = ctx.At(ImVec2(center.x + half.x, center.y + half.y));

            ImTextureID tex = textures.Find(item.display_name);
            if (!tex && item.thumb_ready && !item.thumbnail.empty()) {
                tex = textures.FromRgba(item.display_name, item.thumbnail.data(),
                                        static_cast<int>(item.thumb_width),
                                        static_cast<int>(item.thumb_height));
                model.gallery->MarkUploaded(item.path);
            } else if (!tex && item.thumb_uploaded) {
                model.gallery->ReloadThumbnail(item.path);
            }

            if (tex)
                ctx.dl->AddImageRounded(tex, a, b, ImVec2(0, 0), ImVec2(1, 1),
                                        ctx.Fade(IM_COL32_WHITE), 12.0f);
            else
                ctx.dl->AddRectFilled(a, b, ctx.Fade(IM_COL32(255, 255, 255, 20)), 12.0f);

            DrawThumbBadge(ctx, ImVec2(tile_pos.x + 5.0f, tile_pos.y + 5.0f),
                           item.duration_label.c_str());

            if (it.clicked) OpenInPlayer(model, item.path);
            tx += tile + 10.0f;
        }

        if (items.empty())
            DrawText(ctx, Font::Small, ImVec2(pos.x + 14.0f, pos.y + kGalleryStripH * 0.5f - 10.0f),
                     kTextMuted, "Записей пока нет");

        ctx.dl->PopClipRect();

        const float fade_x = pos.x + col.w - 90.0f;
        ctx.dl->AddRectFilledMultiColor(ctx.At(ImVec2(fade_x, pos.y)),
                                        ctx.At(ImVec2(pos.x + col.w, pos.y + kGalleryStripH)),
                                        IM_COL32(27, 27, 27, 0), ctx.Fade(kPanelBg),
                                        ctx.Fade(kPanelBg), IM_COL32(27, 27, 27, 0));

        if (DiscButton(ctx, HashId("gallery-open"), open_pos, 44.0f, "arrow-circle", 44.0f,
                       IM_COL32(0, 0, 0, 0), kText, false))
            Navigate(Page::Gallery, true);
    }

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "запись");

    {

        const ImVec2 pos = col.Next(kRowH_Badge);
        Interaction row = Row(ctx, HashId("row-record"), pos, ImVec2(col.w, kRowH_Badge));
        RowIcon(ctx, row, pos, "record-circle");
        RowTitle(ctx, pos, model.recording ? "Идёт запись" : "Запись");
        RowSubtitle(ctx, pos, model.recording ? "Остановить запись" : "Активировать запись", 38.0f);
        DrawBadge(ctx, ImVec2(pos.x + kRowTextX, pos.y + 62.0f), model.record_hotkey.c_str());

        if (DiscButton(ctx, HashId("btn-record"),
                       ImVec2(pos.x + col.w - 62.0f, pos.y + (kRowH_Badge - kCircleBtnSize) * 0.5f),
                       kCircleBtnSize, model.recording ? "record-dot" : "video-circle", 40.0f,
                       kCircleBtn, model.recording ? kDanger : kText))
            if (model.on_toggle_record) model.on_toggle_record();
    }

    {
        const ImVec2 pos = col.Next(kRowH_Badge);
        Interaction row = Row(ctx, HashId("row-replay"), pos, ImVec2(col.w, kRowH_Badge));
        RowIcon(ctx, row, pos, "repeat-circle");
        RowTitle(ctx, pos, "Откаты");
        RowSubtitle(ctx, pos, "Мгновенные повторы", 38.0f);
        DrawBadge(ctx, ImVec2(pos.x + kRowTextX, pos.y + 62.0f), model.replay_hotkey.c_str());

        bool enabled = model.replay_armed;
        if (Toggle(ctx, HashId("toggle-replay"),
                   ImVec2(pos.x + col.w - 95.0f, pos.y + (kRowH_Badge - kToggleH) * 0.5f), enabled))
            if (model.on_set_replay) model.on_set_replay(enabled);
    }

    {
        const ImVec2 pos(panel_min.x + kPanelPad,
                         panel_max.y - kPanelPad - 15.0f - kItemGap - kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("row-settings"), pos, ImVec2(col.w, kRowH_TwoLine), true);
        RowIcon(ctx, row, pos, "setting-3");
        RowTitle(ctx, pos, "Настройки");
        RowSubtitle(ctx, pos, "Открыть настройки reframe++", 41.0f);

        const bool btn = CircleButton(ctx, HashId("btn-settings"),
                                      ImVec2(pos.x + col.w - 62.0f, pos.y + 10.0f), kCircleBtnSize,
                                      "arrow-circle");
        if (row.clicked || btn) Navigate(Page::Settings, true);
    }

}

void Menu::PageSettings(UiContext& ctx, ImVec2 panel_min, [[maybe_unused]] ImVec2 panel_max,
                        AppModel& model) {
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;
    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "настройки");

    struct Entry {
        const char* icon;
        const char* title;
        const char* subtitle;
        Page page;
        const char* id;
    };
    static constexpr Entry kEntries[] = {
        {"monitor-recorder", "Видео", "Настройки записи видео", Page::Video, "nav-video"},
        {"sound", "Аудио", "Настройки записи аудио", Page::Audio, "nav-audio"},
        {"driver-2", "Диск", "Настройки хранилища", Page::Disk, "nav-disk"},
        {"setting-3", "Интерфейс", "Настройки интерфейса", Page::Interface, "nav-ui"},
        {"driver-2", "Горячие клавиши", "Настройки кейбиндов", Page::Keybinds, "nav-keys"},
    };

    for (const Entry& entry : kEntries) {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId(entry.id), pos, ImVec2(col.w, kRowH_TwoLine), true);
        RowIcon(ctx, row, pos, entry.icon);
        RowTitle(ctx, pos, entry.title);
        RowSubtitle(ctx, pos, entry.subtitle, 41.0f);

        const bool btn = CircleButton(ctx, HashId(entry.id, 1),
                                      ImVec2(pos.x + col.w - 62.0f, pos.y + 10.0f), kCircleBtnSize,
                                      "arrow-circle");
        if (row.clicked || btn) Navigate(entry.page, true);
    }

}

void Menu::PageVideo(UiContext& ctx, ImVec2 panel_min, [[maybe_unused]] ImVec2 panel_max,
                       AppModel& model) {
    Settings& s = *model.settings;
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;
    bool dirty = false;

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "видео");

    {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("v-desktop"), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, "monitor-recorder");
        RowTitle(ctx, pos, "Запись рабочего стола");
        RowSubtitle(ctx, pos, "Захватывать весь экран", 41.0f);

        bool desktop = !s.capture_focused_window_only;
        if (Toggle(ctx, HashId("v-desktop-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f),
                   desktop)) {
            s.capture_focused_window_only = !desktop;

            s.ClampFpsToDisplay(model.display_hz);
            dirty = true;
        }
    }

    {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("v-cursor"), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, "gallery");
        RowTitle(ctx, pos, "Рисовать курсор");
        RowSubtitle(ctx, pos, "Указатель мыши в записи", 41.0f);

        bool cursor = s.capture_cursor;
        if (Toggle(ctx, HashId("v-cursor-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f),
                   cursor)) {
            s.capture_cursor = cursor;
            dirty = true;
        }
    }

    {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId("v-replay"), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, "repeat-circle");
        RowTitle(ctx, pos, "Длительность повтора");

        const double megabytes =
            (s.bitrate_kbps + s.audio_bitrate_kbps * (s.separate_audio_tracks() ? 2.0 : 1.0)) *
            s.replay_seconds / 8.0 / 1024.0;
        const std::string size = megabytes >= 1024.0
                                     ? TrFormat("{:.1f} ГБ", megabytes / 1024.0)
                                     : TrFormat("{:.0f} МБ", megabytes);
        RowSubtitle(ctx, pos, TrFormat("Примерно {} на клип", size).c_str());

        float seconds = static_cast<float>(s.replay_seconds);
        const std::string label = FormatHms(seconds);
        if (Slider(ctx, HashId("v-replay-s"), pos, col.w, seconds, 15.0f, 1200.0f, label.c_str())) {
            s.replay_seconds = static_cast<std::uint32_t>(seconds + 0.5f);
            dirty = true;
        }
    }

    if (model.monitors.size() > 1 && !s.capture_focused_window_only) {
        {
            const ImVec2 pos = col.Next(kRowH_TwoLine);
            Interaction row = Row(ctx, HashId("v-follow"), pos, ImVec2(col.w, kRowH_TwoLine));
            RowIcon(ctx, row, pos, "monitor");
            RowTitle(ctx, pos, "Следовать за курсором");
            RowSubtitle(ctx, pos, "Экран меняется за курсором", 41.0f);

            bool follow = s.monitor_follow_cursor;
            if (Toggle(ctx, HashId("v-follow-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f),
                       follow)) {
                s.monitor_follow_cursor = follow;
                dirty = true;
            }
        }

        if (s.monitor_follow_cursor) {
            const ImVec2 pos = col.Next(kRowH_Slider);
            Interaction row = Row(ctx, HashId("v-follow-delay"), pos, ImVec2(col.w, kRowH_Slider));
            RowIcon(ctx, row, pos, "speedometer");
            RowTitle(ctx, pos, "Задержка переключения");
            RowSubtitle(ctx, pos, "Пауза перед сменой экрана");

            float seconds = static_cast<float>(s.monitor_switch_delay_ms) / 1000.0f;
            const std::string label =
                seconds < 0.05f ? std::string(Tr("сразу")) : TrFormat("{:.1f} с", seconds);
            if (Slider(ctx, HashId("v-follow-delay-s"), pos, col.w, seconds, 0.0f, 10.0f,
                       label.c_str())) {
                s.monitor_switch_delay_ms = static_cast<std::uint32_t>(seconds * 1000.0f + 0.5f);
                dirty = true;
            }
        }

        const bool locked = s.monitor_follow_cursor;
        constexpr float kItemH = 36.0f;
        const float row_h =
            kRowH_TwoLine +
            (monitor_list_open_ ? static_cast<float>(model.monitors.size()) * kItemH + 10.0f : 0.0f);

        std::size_t selected = 0;
        for (std::size_t i = 0; i < model.monitors.size(); ++i)
            if (model.monitors[i].device_name == s.capture_monitor) selected = i;

        const ImVec2 pos = col.Next(row_h);
        ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(ImVec2(pos.x + col.w, pos.y + row_h)),
                              ctx.Fade(kRowBg), kRowRadius);

        Interaction head = locked ? Interaction{}
                                  : Hit(ctx, HashId("v-monitor"), pos,
                                        ImVec2(pos.x + col.w, pos.y + kRowH_TwoLine));
        const ImU32 text_col = locked ? kTextMuted : kText;
        DrawIcon(ctx, "monitor-recorder", ImVec2(pos.x + kRowIconX, pos.y + kRowIconY), kRowIcon,
                 text_col, 1.0f + 0.10f * head.hover);
        DrawText(ctx, Font::Body, ImVec2(pos.x + kRowTextX, pos.y + 14.0f), text_col, "Монитор");
        RowSubtitle(ctx, pos,
                    ElideEnd(ctx, Font::Small,
                             locked ? std::string("Выбирается автоматически")
                                    : model.monitors[selected].name,
                             col.w - kRowTextX - 44.0f)
                        .c_str(),
                    41.0f);

        if (!locked) {
            Spring& turn = ctx.anim->Get(HashId("v-monitor-turn"));
            turn.SetTarget(monitor_list_open_ ? 1.0f : 0.0f);
            const float t = turn.Update(ctx.dt, spring::kMenu);
            const ImVec2 ch_center(pos.x + col.w - 28.0f, pos.y + kRowH_TwoLine * 0.5f);
            const float angle = t * 1.5707963f;
            const float ca = std::cos(angle), sa = std::sin(angle);
            auto rot = [&](float x, float y) {
                return ctx.At(ImVec2(ch_center.x + x * ca - y * sa, ch_center.y + x * sa + y * ca));
            };
            const ImVec2 chevron_pts[3] = {rot(-2.5f, -5.0f), rot(2.5f, 0.0f), rot(-2.5f, 5.0f)};
            ctx.dl->AddPolyline(
                chevron_pts, 3,
                ctx.Fade(IM_COL32(255, 255, 255, 140 + static_cast<int>(100 * head.hover))), 0,
                2.0f);
        }

        if (head.clicked) monitor_list_open_ = !monitor_list_open_;

        if (monitor_list_open_ && !locked) {
            float y = pos.y + kRowH_TwoLine + 4.0f;
            for (std::size_t i = 0; i < model.monitors.size(); ++i) {
                const ImVec2 item_pos(pos.x + 10.0f, y);
                const ImVec2 item_max(pos.x + col.w - 10.0f, y + kItemH);
                Interaction it = Hit(ctx, HashId("v-monitor-item", static_cast<std::uint32_t>(i)),
                                     item_pos, item_max);
                if (it.hover > 0.01f)
                    ctx.dl->AddRectFilled(
                        ctx.At(item_pos), ctx.At(item_max),
                        ctx.Fade(IM_COL32(255, 255, 255, static_cast<int>(18 * it.hover))), 10.0f);

                if (i == selected)
                    ctx.dl->AddCircleFilled(ctx.At(ImVec2(item_pos.x + 14.0f, y + kItemH * 0.5f)),
                                            3.5f, ctx.Fade(kAccent), 12);

                DrawText(ctx, Font::Small, ImVec2(item_pos.x + 28.0f, y + 9.0f),
                         i == selected ? kText : kTextMuted,
                         ElideEnd(ctx, Font::Small, model.monitors[i].name,
                                  col.w - 20.0f - 28.0f - 10.0f)
                             .c_str());

                if (it.clicked) {
                    s.capture_monitor = model.monitors[i].device_name;
                    monitor_list_open_ = false;
                    dirty = true;
                }
                y += kItemH;
            }
        }

        if (monitor_list_open_) {
            if (dirty && model.on_settings_changed) model.on_settings_changed();
            return;
        }
    }

    if (model.gpus.size() > 1) {
        constexpr float kItemH = 36.0f;
        const float row_h = kRowH_TwoLine +
                            (gpu_list_open_
                                 ? static_cast<float>(model.gpus.size()) * kItemH + 10.0f
                                 : 0.0f);

        std::size_t selected = 0;
        for (std::size_t i = 1; i < model.gpus.size(); ++i)
            if (model.gpus[i].luid_low == s.gpu_luid_low &&
                model.gpus[i].luid_high == s.gpu_luid_high)
                selected = i;

        const ImVec2 pos = col.Next(row_h);
        ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(ImVec2(pos.x + col.w, pos.y + row_h)),
                              ctx.Fade(kRowBg), kRowRadius);

        Interaction head =
            Hit(ctx, HashId("v-gpu"), pos, ImVec2(pos.x + col.w, pos.y + kRowH_TwoLine));
        DrawIcon(ctx, "driver", ImVec2(pos.x + kRowIconX, pos.y + kRowIconY), kRowIcon, kText,
                 1.0f + 0.10f * head.hover);
        RowTitle(ctx, pos, "Видеокарта");
        RowSubtitle(ctx, pos,
                    ElideEnd(ctx, Font::Small, model.gpus[selected].name,
                             col.w - kRowTextX - 44.0f)
                        .c_str(),
                    41.0f);

        Spring& turn = ctx.anim->Get(HashId("v-gpu-turn"));
        turn.SetTarget(gpu_list_open_ ? 1.0f : 0.0f);
        const float t = turn.Update(ctx.dt, spring::kMenu);
        const ImVec2 ch_center(pos.x + col.w - 28.0f, pos.y + kRowH_TwoLine * 0.5f);
        const float angle = t * 1.5707963f;
        const float ca = std::cos(angle), sa = std::sin(angle);
        auto rot = [&](float x, float y) {
            return ctx.At(ImVec2(ch_center.x + x * ca - y * sa, ch_center.y + x * sa + y * ca));
        };
        const ImVec2 chevron_pts[3] = {rot(-2.5f, -5.0f), rot(2.5f, 0.0f), rot(-2.5f, 5.0f)};
        ctx.dl->AddPolyline(
            chevron_pts, 3,
            ctx.Fade(IM_COL32(255, 255, 255, 140 + static_cast<int>(100 * head.hover))), 0, 2.0f);

        if (head.clicked) gpu_list_open_ = !gpu_list_open_;

        if (gpu_list_open_) {
            float y = pos.y + kRowH_TwoLine + 4.0f;
            for (std::size_t i = 0; i < model.gpus.size(); ++i) {
                const ImVec2 item_pos(pos.x + 10.0f, y);
                const ImVec2 item_max(pos.x + col.w - 10.0f, y + kItemH);
                Interaction it =
                    Hit(ctx, HashId("v-gpu-item", static_cast<std::uint32_t>(i)), item_pos, item_max);
                if (it.hover > 0.01f)
                    ctx.dl->AddRectFilled(
                        ctx.At(item_pos), ctx.At(item_max),
                        ctx.Fade(IM_COL32(255, 255, 255, static_cast<int>(18 * it.hover))), 10.0f);

                if (i == selected)
                    ctx.dl->AddCircleFilled(ctx.At(ImVec2(item_pos.x + 14.0f, y + kItemH * 0.5f)),
                                            3.5f, ctx.Fade(kAccent), 12);

                DrawText(ctx, Font::Small, ImVec2(item_pos.x + 28.0f, y + 9.0f),
                         i == selected ? kText : kTextMuted,
                         ElideEnd(ctx, Font::Small, model.gpus[i].name,
                                  col.w - 20.0f - 28.0f - 10.0f)
                             .c_str());

                if (it.clicked) {
                    s.gpu_luid_low = model.gpus[i].luid_low;
                    s.gpu_luid_high = model.gpus[i].luid_high;
                    gpu_list_open_ = false;
                    dirty = true;
                }
                y += kItemH;
            }
        }
    }

    if (gpu_list_open_) {
        if (dirty && model.on_settings_changed) model.on_settings_changed();
        return;
    }

    DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "формат вывода");

    if (s.ResolvedFps() >= 120) {
        const ImVec2 pos = col.Peek();
        col.Skip(WarningRow(ctx, pos, col.w, kWarnRed, kWarnRedBg,
                            "Запись в 120+ FPS может сказаться на производительности!"));
    }
    if (s.fps_clamped_to_display) {
        const ImVec2 pos = col.Peek();
        col.Skip(WarningRow(ctx, pos, col.w, kWarnAmber, kWarnAmberBg,
                            "При включенной \"Записи рабочего стола\" FPS не может "
                            "привышать герцовку вашего монитора!"));
    }

    struct StepperRow {
        const char* id;
        const char* icon;
        const char* title;
        std::uint32_t* value;
        const char* const* names;
        int count;
    };
    const StepperRow kSteppers[] = {

        {"v-quality", "medal-star", "Качество", &s.quality, kQualityNames,
         static_cast<int>(kQualityCustom)},
        {"v-res", "monitor", "Разрешение", &s.resolution, kResolutionNames,
         static_cast<int>(std::size(kResolutionNames))},
        {"v-fps", "video", "Частота кадров", &s.fps_option, kFpsNames,
         static_cast<int>(std::size(kFpsNames))},
    };

    for (const StepperRow& row_def : kSteppers) {
        const ImVec2 pos = col.Next(kRowH_Simple);
        Interaction row = Row(ctx, HashId(row_def.id), pos, ImVec2(col.w, kRowH_Simple));
        DrawIcon(ctx, row_def.icon, ImVec2(pos.x + kRowIconX, pos.y + 16.0f), kRowIcon, kText,
                 1.0f + 0.10f * row.hover);
        DrawText(ctx, Font::Body, ImVec2(pos.x + kRowTextX, pos.y + 15.0f), kText, row_def.title);

        const auto current = std::min<std::uint32_t>(
            *row_def.value, static_cast<std::uint32_t>(std::size(kQualityNames) - 1));
        int index = (row_def.value == &s.quality) ? static_cast<int>(current)
                                                  : static_cast<int>(current) % row_def.count;
        const char* label = (row_def.value == &s.quality) ? kQualityNames[index]
                                                          : row_def.names[index];

        if (Stepper(ctx, HashId(row_def.id, 7), pos, col.w, pos.y + kRowH_Simple * 0.5f, index,
                    row_def.count, label)) {
            *row_def.value = static_cast<std::uint32_t>(index);
            s.ApplyQualityPreset();

            if (row_def.value == &s.fps_option) {
                s.fps_clamped_to_display = false;
                s.ClampFpsToDisplay(model.display_hz);
            }
            dirty = true;
        }
    }

    {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId("v-bitrate"), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, "speedometer");
        RowTitle(ctx, pos, "Скорость передачи");
        RowSubtitle(ctx, pos, "Битрейт видеопотока, Мбит/с");

        float mbps = static_cast<float>(s.bitrate_kbps) / 1000.0f;
        const std::string label = std::format("{}", static_cast<int>(mbps + 0.5f));
        if (Slider(ctx, HashId("v-bitrate-s"), pos, col.w, mbps,
                   static_cast<float>(kBitrateMinKbps) / 1000.0f,
                   static_cast<float>(kBitrateMaxKbps) / 1000.0f, label.c_str())) {
            s.bitrate_kbps = static_cast<std::uint32_t>(mbps * 1000.0f + 0.5f);

            s.quality = kQualityCustom;
            dirty = true;
        }
    }

    if (dirty && model.on_settings_changed) model.on_settings_changed();
}

void Menu::PageAudio(UiContext& ctx, ImVec2 panel_min, [[maybe_unused]] ImVec2 panel_max,
                       AppModel& model) {
    Settings& s = *model.settings;
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;
    bool dirty = false;

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "аудио");
    DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "системные звуки");

    auto volume_row = [&](const char* id, const char* icon, const char* title, const char* subtitle,
                          float& value, bool enabled = true) {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId(id), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, (!enabled || value <= 0.001f) ? "volume-off" : icon);
        RowTitle(ctx, pos, title);
        RowSubtitle(ctx, pos, subtitle);

        float percent = value * 100.0f;
        const std::string label = std::format("{} %", static_cast<int>(percent + 0.5f));
        if (Slider(ctx, HashId(id, 3), pos, col.w, percent, 0.0f, 100.0f, label.c_str())) {
            value = percent / 100.0f;
            dirty = true;
        }
    };

    {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("a-sys-on"), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, s.record_system_audio ? "sound" : "volume-off");
        RowTitle(ctx, pos, "Записывать");
        RowSubtitle(ctx, pos, "Звук системы в записи", 41.0f);

        bool on = s.record_system_audio;
        if (Toggle(ctx, HashId("a-sys-on-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f), on)) {
            s.record_system_audio = on;
            dirty = true;
        }
    }

    volume_row("a-sys", "speaker", "Громкость", "Звук игры и приложений", s.system_volume,
               s.record_system_audio);

    DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "микрофон");

    {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("a-mic-on"), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, s.record_microphone ? "microphone" : "volume-off");
        RowTitle(ctx, pos, "Записывать");
        RowSubtitle(ctx, pos, "Голос в записи", 41.0f);

        bool on = s.record_microphone;
        if (Toggle(ctx, HashId("a-mic-on-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f), on)) {
            s.record_microphone = on;
            dirty = true;
        }
    }

    {

        constexpr float kItemH = 36.0f;
        const int item_count = static_cast<int>(model.mic_devices.size()) + 1;
        const float row_h =
            kRowH_TwoLine + (mic_list_open_ ? item_count * kItemH + 10.0f : 0.0f);

        const ImVec2 pos = col.Next(row_h);
        ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(ImVec2(pos.x + col.w, pos.y + row_h)),
                              ctx.Fade(kRowBg), kRowRadius);

        Interaction head =
            Hit(ctx, HashId("a-source"), pos, ImVec2(pos.x + col.w, pos.y + kRowH_TwoLine));
        RowIcon(ctx, head, pos, "microphone");
        RowTitle(ctx, pos, "Источник");
        const std::string label =
            ElideEnd(ctx, Font::Small, model.mic_device_label, col.w - kRowTextX - 44.0f);
        RowSubtitle(ctx, pos, label.c_str(), 41.0f);

        Spring& turn = ctx.anim->Get(HashId("a-source-turn"));
        turn.SetTarget(mic_list_open_ ? 1.0f : 0.0f);
        const float t = turn.Update(ctx.dt, spring::kMenu);
        const ImVec2 ch_center(pos.x + col.w - 28.0f, pos.y + kRowH_TwoLine * 0.5f);
        const float angle = t * 1.5707963f;
        const float c = std::cos(angle), s2 = std::sin(angle);
        auto rot = [&](float x, float y) {
            return ctx.At(ImVec2(ch_center.x + x * c - y * s2, ch_center.y + x * s2 + y * c));
        };
        const ImVec2 chevron_pts[3] = {rot(-2.5f, -5.0f), rot(2.5f, 0.0f), rot(-2.5f, 5.0f)};
        ctx.dl->AddPolyline(chevron_pts, 3,
                            ctx.Fade(IM_COL32(255, 255, 255,
                                              140 + static_cast<int>(100 * head.hover))),
                            0, 2.0f);

        if (head.clicked) mic_list_open_ = !mic_list_open_;

        if (mic_list_open_) {
            float y = pos.y + kRowH_TwoLine + 4.0f;
            std::uint32_t salt = 0;
            auto device_line = [&](const std::string& id, const std::string& name) {
                const ImVec2 item_pos(pos.x + 10.0f, y);
                const ImVec2 item_max(pos.x + col.w - 10.0f, y + kItemH);
                Interaction it = Hit(ctx, HashId("a-mic-item", salt++), item_pos, item_max);
                if (it.hover > 0.01f)
                    ctx.dl->AddRectFilled(
                        ctx.At(item_pos), ctx.At(item_max),
                        ctx.Fade(IM_COL32(255, 255, 255, static_cast<int>(18 * it.hover))), 10.0f);

                const bool selected = id == model.mic_selected_id;
                if (selected)
                    ctx.dl->AddCircleFilled(ctx.At(ImVec2(item_pos.x + 14.0f, y + kItemH * 0.5f)),
                                            3.5f, ctx.Fade(kAccent), 12);

                const std::string item_label =
                    ElideEnd(ctx, Font::Small, name, col.w - 20.0f - 28.0f - 10.0f);
                DrawText(ctx, Font::Small, ImVec2(item_pos.x + 28.0f, y + 9.0f),
                         selected ? kText : kTextMuted, item_label.c_str());

                if (it.clicked) {

                    if (!s.record_microphone) {
                        s.record_microphone = true;
                        dirty = true;
                    }
                    if (model.on_pick_mic) model.on_pick_mic(id);
                    mic_list_open_ = false;
                }
                y += kItemH;
            };

            device_line("", "Системный по умолчанию");
            for (const auto& mic : model.mic_devices) device_line(mic.id, mic.name);
        }
    }

    if (!mic_list_open_) {
        volume_row("a-mic", "volume-high", "Громкость", "Уровень входного сигнала", s.mic_volume,
                   s.record_microphone);
        volume_row("a-gain", "activity", "Усиление", "Дополнительный буст голоса", s.mic_gain,
                   s.record_microphone);

        {
            const ImVec2 pos = col.Next(kRowH_TwoLine);
            Interaction row = Row(ctx, HashId("a-ns"), pos, ImVec2(col.w, kRowH_TwoLine));
            RowIcon(ctx, row, pos, "voice-square");
            RowTitle(ctx, pos, "Шумоподавление");
            RowSubtitle(ctx, pos, "Шумоподавление микрофона", 41.0f);

            bool on = s.mic_noise_suppression;
            if (Toggle(ctx, HashId("a-ns-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f), on)) {
                s.mic_noise_suppression = on;
                dirty = true;
            }
        }

        if (s.mic_noise_suppression) {
            const bool maxine_missing =
                s.noise_suppression == NoiseSuppression::Maxine && !model.maxine_installed;
            constexpr float kItemH = 36.0f;
            const int mode_count = static_cast<int>(std::size(kNoiseSuppressionNames));
            const float row_h =
                kRowH_TwoLine + (noise_list_open_ ? mode_count * kItemH + 10.0f : 0.0f);
            const ImVec2 pos = col.Next(row_h);
            ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(ImVec2(pos.x + col.w, pos.y + row_h)),
                                  ctx.Fade(kRowBg), kRowRadius);

            Interaction head =
                Hit(ctx, HashId("a-ns-mode"), pos, ImVec2(pos.x + col.w, pos.y + kRowH_TwoLine));
            RowIcon(ctx, head, pos, "voice-square");
            RowTitle(ctx, pos, "Режим шумоподавления");
            const auto mode = static_cast<std::size_t>(s.noise_suppression);
            RowSubtitle(ctx, pos, kNoiseSuppressionNames[mode], 41.0f);
            if (maxine_missing) {
                const float title_w = MeasureText(ctx, Font::Body, "Режим шумоподавления").x;
                DrawIcon(ctx, "danger", ImVec2(pos.x + kRowTextX + title_w + 8.0f, pos.y + 12.0f),
                         20.0f, kWarnAmber);
            }

            Spring& turn = ctx.anim->Get(HashId("a-ns-turn"));
            turn.SetTarget(noise_list_open_ ? 1.0f : 0.0f);
            const float t = turn.Update(ctx.dt, spring::kMenu);
            const ImVec2 ch_center(pos.x + col.w - 28.0f, pos.y + kRowH_TwoLine * 0.5f);
            const float c = std::cos(t * 1.5707963f), sn = std::sin(t * 1.5707963f);
            auto rot = [&](float x, float y) {
                return ctx.At(ImVec2(ch_center.x + x * c - y * sn, ch_center.y + x * sn + y * c));
            };
            const ImVec2 chevron_pts[3] = {rot(-2.5f, -5.0f), rot(2.5f, 0.0f), rot(-2.5f, 5.0f)};
            ctx.dl->AddPolyline(chevron_pts, 3,
                                ctx.Fade(IM_COL32(255, 255, 255,
                                                  140 + static_cast<int>(100 * head.hover))),
                                0, 2.0f);
            if (head.clicked) noise_list_open_ = !noise_list_open_;

            if (noise_list_open_) {
                float y = pos.y + kRowH_TwoLine + 4.0f;
                for (int i = 0; i < mode_count; ++i) {
                    const ImVec2 item_pos(pos.x + 10.0f, y);
                    const ImVec2 item_max(pos.x + col.w - 10.0f, y + kItemH);
                    Interaction it = Hit(ctx, HashId("a-ns-item", static_cast<std::uint32_t>(i)),
                                         item_pos, item_max);
                    if (it.hover > 0.01f)
                        ctx.dl->AddRectFilled(
                            ctx.At(item_pos), ctx.At(item_max),
                            ctx.Fade(IM_COL32(255, 255, 255, static_cast<int>(18 * it.hover))),
                            10.0f);
                    const bool selected = static_cast<std::size_t>(i) == mode;
                    if (selected)
                        ctx.dl->AddCircleFilled(
                            ctx.At(ImVec2(item_pos.x + 14.0f, y + kItemH * 0.5f)), 3.5f,
                            ctx.Fade(kAccent), 12);
                    DrawText(ctx, Font::Small, ImVec2(item_pos.x + 28.0f, y + 9.0f),
                             selected ? kText : kTextMuted, kNoiseSuppressionNames[i]);
                    if (it.clicked) {
                        s.noise_suppression = static_cast<NoiseSuppression>(i);
                        noise_list_open_ = false;
                        dirty = true;
                    }
                    y += kItemH;
                }
            }

            if (maxine_missing) {
                ActionWarning warning;
                warning.text = "Прежде чем использовать NVIDIA Maxine";
                warning.emphasis = "требуется его загрузить";
                warning.button = model.maxine_downloading ? "Загрузка..." : "Загрузить";
                warning.enabled = !model.maxine_downloading;
                bool pressed = false;
                const ImVec2 warn = col.Peek();
                col.Skip(WarningActionRow(ctx, HashId("a-ns-get"), warn, col.w, kWarnAmber,
                                          kWarnAmberBg, warning, pressed));
                if (pressed && model.on_download_maxine) model.on_download_maxine();
            }
        }

        DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "аудиодорожки");

        const ImVec2 pos = col.Next(kRowH_Format);
        Interaction row = Row(ctx, HashId("a-format"), pos, ImVec2(col.w, kRowH_Format));
        RowIcon(ctx, row, pos, "audio-square");
        RowTitle(ctx, pos, "Формат");

        int index = static_cast<int>(s.audio_tracks) % static_cast<int>(std::size(kAudioTrackNames));
        if (Stepper(ctx, HashId("a-format-s"), pos, col.w, pos.y + 25.0f, index,
                    static_cast<int>(std::size(kAudioTrackNames)), kAudioTrackNames[index])) {
            s.audio_tracks = static_cast<std::uint32_t>(index);
            dirty = true;
        }

        const bool single = s.audio_tracks == 0;
        static constexpr const char* kLayoutLines[][2] = {
            {"Звук системы и микрофона", "на одной дорожке"},
            {"Общая, система", "и микрофон отдельно"},
            {"Общая, микрофон и", "каждое приложение"},
        };
        const auto& lines = kLayoutLines[std::min<std::size_t>(s.audio_tracks, 2)];
        RowSubtitle(ctx, pos, lines[0], 48.0f);
        RowSubtitle(ctx, pos, lines[1], 70.0f);

        if (!single && !s.record_microphone) {
            const ImVec2 warn = col.Peek();
            col.Skip(WarningRow(ctx, warn, col.w, kWarnAmber, kWarnAmberBg,
                                "Дорожка микрофона появится только с включённым микрофоном!"));
        }

        if (s.app_audio_tracks()) {
            const ImVec2 pos = col.Next(kRowH_Format);
            Interaction row = Row(ctx, HashId("a-apps-n"), pos, ImVec2(col.w, kRowH_Format));
            RowIcon(ctx, row, pos, "data");
            RowTitle(ctx, pos, "Количество дорожек");

            const int span = static_cast<int>(kAppTrackSlotsMax - kAppTrackSlotsMin) + 1;
            int index = static_cast<int>(s.app_track_slots - kAppTrackSlotsMin);
            const std::string label = std::format("{}", s.app_track_slots);
            if (Stepper(ctx, HashId("a-apps-n-s"), pos, col.w, pos.y + 25.0f, index, span,
                        label.c_str())) {
                s.app_track_slots = kAppTrackSlotsMin + static_cast<std::uint32_t>(index);
                dirty = true;
            }
            RowSubtitle(ctx, pos, "Лишние приложения", 48.0f);
            RowSubtitle(ctx, pos, "слышны в общей дорожке", 70.0f);

            if (!s.record_system_audio) {
                const ImVec2 warn = col.Peek();
                col.Skip(WarningRow(ctx, warn, col.w, kWarnAmber, kWarnAmberBg,
                                    "Дорожки приложений пишутся только вместе со звуком системы!"));
            }
        }
    }

    if (dirty && model.on_settings_changed) model.on_settings_changed();
}

void Menu::PageInterface(UiContext& ctx, ImVec2 panel_min, [[maybe_unused]] ImVec2 panel_max,
                         AppModel& model) {
    Settings& s = *model.settings;
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;
    bool dirty = false;

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "интерфейс");

    {
        const ImVec2 pos = col.Next(kRowH_Simple);
        Interaction row = Row(ctx, HashId("i-lang"), pos, ImVec2(col.w, kRowH_Simple));
        RowIcon(ctx, row, pos, "medal-star", kRowIconY + 5.0f);
        RowTitle(ctx, pos, "Язык", kRowTitleY + 5.0f);

        int index = s.language == Language::English ? 1 : 0;
        if (Stepper(ctx, HashId("i-lang-s"), pos, col.w, pos.y + kRowH_Simple * 0.5f, index,
                    static_cast<int>(std::size(kLanguageNames)), kLanguageNames[index])) {
            s.language = index == 1 ? Language::English : Language::Russian;
            SetLanguage(s.language);
            dirty = true;
        }
    }

    {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId("i-scale"), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, "gallery");
        RowTitle(ctx, pos, "Размер интерфейса");
        RowSubtitle(ctx, pos, "Масштаб меню и подсказок");

        float percent = s.ui_scale * 100.0f;
        const std::string label = std::format("{}%", static_cast<int>(percent + 0.5f));
        if (Slider(ctx, HashId("i-scale-s"), pos, col.w, percent, kUiScaleMin * 100.0f,
                   kUiScaleMax * 100.0f, label.c_str())) {
            s.ui_scale = std::round(percent / 5.0f) * 5.0f / 100.0f;
            dirty = true;
        }
    }

    DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "запись");

    auto toggle_row = [&](const char* id, const char* icon, const char* title, const char* subtitle,
                          bool& value) {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId(id), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, icon);
        RowTitle(ctx, pos, title);
        RowSubtitle(ctx, pos, subtitle, 41.0f);
        if (Toggle(ctx, HashId(id, 1), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f), value))
            dirty = true;
    };

    toggle_row("i-rec", "record-circle", "Индикатор записи", "Кружок и таймер поверх игры",
               s.show_record_indicator);
    if (s.show_record_indicator)
        toggle_row("i-stop", "close", "Кнопка остановки", "Рядом с индикатором записи",
                   s.show_stop_button);

    DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "значки");

    toggle_row("i-mic", "microphone", "Индикатор микрофона", "Значок в углу экрана",
               s.show_mic_indicator);
    toggle_row("i-replay", "repeat-circle", "Индикатор повтора", "Виден, пока повтор заряжен",
               s.show_replay_indicator);

    const bool badges = s.show_mic_indicator || s.show_replay_indicator;

    if (badges) {
        const ImVec2 pos = col.Next(kRowH_Simple);
        Interaction row = Row(ctx, HashId("i-corner"), pos, ImVec2(col.w, kRowH_Simple));
        RowIcon(ctx, row, pos, "monitor-recorder", kRowIconY + 5.0f);
        RowTitle(ctx, pos, "Расположение", kRowTitleY + 5.0f);

        int index = static_cast<int>(s.hud_corner) % static_cast<int>(std::size(kHudCornerNames));
        if (Stepper(ctx, HashId("i-corner-s"), pos, col.w, pos.y + kRowH_Simple * 0.5f, index,
                    static_cast<int>(std::size(kHudCornerNames)), kHudCornerNames[index])) {
            s.hud_corner = static_cast<std::uint32_t>(index);
            dirty = true;
        }
    }

    if (badges) {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId("i-size"), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, "monitor-recorder");
        RowTitle(ctx, pos, "Размер значков");
        RowSubtitle(ctx, pos, "От 60% до 200%");

        float percent = s.hud_badge_scale * 100.0f;
        const std::string label = std::format("{}%", static_cast<int>(percent + 0.5f));
        if (Slider(ctx, HashId("i-size-s"), pos, col.w, percent, kHudBadgeScaleMin * 100.0f,
                   kHudBadgeScaleMax * 100.0f, label.c_str())) {
            s.hud_badge_scale = std::round(percent / 5.0f) * 5.0f / 100.0f;
            dirty = true;
        }
    }

    if (badges) {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId("i-alpha"), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, "activity");
        RowTitle(ctx, pos, "Прозрачность");
        RowSubtitle(ctx, pos, "Насколько значки видно");

        float percent = s.hud_opacity * 100.0f;
        const std::string label = std::format("{}%", static_cast<int>(percent + 0.5f));
        if (Slider(ctx, HashId("i-alpha-s"), pos, col.w, percent, kHudOpacityMin * 100.0f, 100.0f,
                   label.c_str())) {
            s.hud_opacity = std::round(percent / 5.0f) * 5.0f / 100.0f;
            dirty = true;
        }
    }

    if (dirty && model.on_settings_changed) model.on_settings_changed();
}

void Menu::PageKeybinds(UiContext& ctx, ImVec2 panel_min, [[maybe_unused]] ImVec2 panel_max,
                        AppModel& model) {
    Settings& s = *model.settings;
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "горячие клавиши");

    struct Bind {
        const char* id;
        const char* icon;
        const char* title;
        std::uint32_t* mods;
        std::uint32_t* vk;
    };
    const Bind kBinds[] = {
        {"kb-open", "home", "Открыть Reframe++", &s.hotkey_overlay_mods, &s.hotkey_overlay_vk},
        {"kb-replay", "repeat-circle", "Сохранить откат", &s.hotkey_save_replay_mods,
         &s.hotkey_save_replay_vk},
        {"kb-record", "record-circle", "Начать/остановить запись", &s.hotkey_toggle_record_mods,
         &s.hotkey_toggle_record_vk},
    };

    if (capture_row_ >= 0 && model.pending_vk != 0) {
        const std::uint32_t vk = model.pending_vk;
        const std::uint32_t mods = model.pending_mods;
        model.pending_vk = 0;
        model.pending_mods = 0;

        const bool is_modifier = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
                                 vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
                                 vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                                 vk == VK_LWIN || vk == VK_RWIN;
        if (vk == VK_ESCAPE) {
            capture_row_ = -1;
        } else if (!is_modifier) {
            const Bind& bind = kBinds[capture_row_];
            const std::uint32_t old_mods = *bind.mods, old_vk = *bind.vk;
            *bind.mods = mods;
            *bind.vk = vk;

            if (model.on_hotkeys_changed && !model.on_hotkeys_changed()) {
                *bind.mods = old_mods;
                *bind.vk = old_vk;
                rejected_row_ = capture_row_;
                if (model.on_hotkeys_changed) model.on_hotkeys_changed();
            } else {
                rejected_row_ = -1;
                if (model.on_settings_changed) model.on_settings_changed();
            }
            capture_row_ = -1;
        }
    }

    for (int i = 0; i < static_cast<int>(std::size(kBinds)); ++i) {
        const Bind& bind = kBinds[i];
        const bool waiting = capture_row_ == i;

        const std::string label = waiting          ? "Жду клавишу..."
                                  : rejected_row_ == i ? "Занято"
                                                       : DescribeHotkey(*bind.mods, *bind.vk);
        const ImVec2 title_size = MeasureText(ctx, Font::Body, bind.title);
        const ImVec2 chip_size = MeasureText(ctx, Font::BadgeItalic, label.c_str());
        const float chip_w = chip_size.x + 6.0f;
        const float chip_h = chip_size.y + 4.0f;

        constexpr float kPad = 10.0f, kGap = 10.0f, kIcon = 24.0f;
        const float inner_w = col.w - 2 * kPad - kIcon - kGap;
        const bool wraps = title_size.x + 10.0f + chip_w > inner_w;
        const float height = wraps ? kPad + title_size.y + 5.0f + chip_h + kPad
                                   : kPad + std::max(kIcon, std::max(title_size.y, chip_h)) + kPad;

        const ImVec2 pos = col.Next(height);
        Interaction row = Row(ctx, HashId(bind.id), pos, ImVec2(col.w, height), true);
        DrawIcon(ctx, bind.icon, ImVec2(pos.x + kPad, pos.y + kPad), kIcon, kText,
                 1.0f + 0.10f * row.hover);

        const float text_x = pos.x + kPad + kIcon + kGap;
        DrawText(ctx, Font::Body, ImVec2(text_x, pos.y + kPad), kText, bind.title);

        const ImVec2 chip_pos =
            wraps ? ImVec2(text_x, pos.y + kPad + title_size.y + 5.0f)
                  : ImVec2(pos.x + col.w - kPad - chip_w, pos.y + (height - chip_h) * 0.5f);

        const ImU32 chip_bg = waiting            ? kAppAccent
                              : rejected_row_ == i ? kWarnRedBg
                                                   : kRowBg;
        ctx.dl->AddRectFilled(ctx.At(chip_pos),
                              ctx.At(ImVec2(chip_pos.x + chip_w, chip_pos.y + chip_h)),
                              ctx.Fade(chip_bg), 5.0f);
        DrawText(ctx, Font::BadgeItalic, ImVec2(chip_pos.x + 3.0f, chip_pos.y + 2.0f),
                 rejected_row_ == i && !waiting ? kWarnRed : kText, label.c_str());

        if (row.clicked) {
            capture_row_ = waiting ? -1 : i;
            rejected_row_ = -1;
            model.pending_vk = 0;
        }
    }

}

void Menu::PageDisk(UiContext& ctx, ImVec2 panel_min, [[maybe_unused]] ImVec2 panel_max,
                        AppModel& model) {
    Settings& s = *model.settings;
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;
    bool dirty = false;

    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "диск");

    {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("d-limit"), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, "driver");
        RowTitle(ctx, pos, "Ограничение места");
        RowSubtitle(ctx, pos, "Лимит на записи", 41.0f);
        if (Toggle(ctx, HashId("d-limit-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f),
                   s.disk_limit_enabled))
            dirty = true;
    }

    if (s.disk_limit_enabled) {
        const ImVec2 pos = col.Next(kRowH_Slider);
        Interaction row = Row(ctx, HashId("d-size"), pos, ImVec2(col.w, kRowH_Slider));
        RowIcon(ctx, row, pos, "data");
        RowTitle(ctx, pos, "Размер хранилища");

        const std::string used = TrFormat("Использовано {} из {}",
                                             FormatGb(model.disk_used_bytes),
                                             FormatGb(model.disk_total_bytes));
        RowSubtitle(ctx, pos, used.c_str());

        constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
        const float max_gb = model.disk_total_bytes > 0
                                 ? static_cast<float>(model.disk_total_bytes / kGiB)
                                 : 2000.0f;

        float gb = std::min(static_cast<float>(s.disk_limit_gb), max_gb);
        const std::string label = TrFormat("{} ГБ", static_cast<int>(gb + 0.5f));
        if (Slider(ctx, HashId("d-size-s"), pos, col.w, gb, 10.0f, max_gb, label.c_str()) ||
            gb != static_cast<float>(s.disk_limit_gb)) {
            s.disk_limit_gb = static_cast<std::uint32_t>(gb + 0.5f);
            dirty = true;
        }
    }

    DrawText(ctx, Font::H2, col.Next(kH2Height), kText, "расположение");

    auto path_row = [&](const char* id, const char* icon, const char* title,
                        std::filesystem::path& path) {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId(id), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, icon);
        RowTitle(ctx, pos, title);

        const float max_w = col.w - kRowTextX - 62.0f - 8.0f;
        const std::wstring wide = path.wstring();
        std::string text = ToUtf8(wide);
        if (MeasureText(ctx, Font::Small, text.c_str()).x > max_w) {
            std::size_t tail = wide.size() > 12 ? wide.size() - 12 : 0;
            while (tail > 6) {
                text = ToUtf8(wide.substr(0, 9)) + "..." + ToUtf8(wide.substr(wide.size() - tail));
                if (MeasureText(ctx, Font::Small, text.c_str()).x <= max_w) break;
                --tail;
            }
        }
        RowSubtitle(ctx, pos, text.c_str(), 41.0f);

        if (DiscButton(ctx, HashId(id, 5), ImVec2(pos.x + col.w - 62.0f, pos.y + 10.0f),
                       kCircleBtnSize, "folder", 24.0f)) {
            std::filesystem::path picked = path;
            if (model.on_pick_folder && model.on_pick_folder(picked)) {
                path = picked;
                dirty = true;
            }
        }
    };

    {
        const ImVec2 pos = col.Next(kRowH_TwoLine);
        Interaction row = Row(ctx, HashId("d-ram"), pos, ImVec2(col.w, kRowH_TwoLine));
        RowIcon(ctx, row, pos, "data");
        RowTitle(ctx, pos, "Хранить откат в ОЗУ");
        RowSubtitle(ctx, pos, "Без записи на диск, быстрее", 41.0f);
        if (Toggle(ctx, HashId("d-ram-t"), ImVec2(pos.x + col.w - 95.0f, pos.y + 13.0f),
                   s.replay_in_memory))
            dirty = true;
    }

    if (!s.replay_in_memory) path_row("d-temp", "document", "Временные файлы", s.temp_dir);
    path_row("d-gallery", "gallery", "Галерея", s.output_dir);

    if (dirty && model.on_settings_changed) model.on_settings_changed();
}

void Menu::PageGallery(UiContext& ctx, ImVec2 panel_min, ImVec2 panel_max, AppModel& model,
                       TextureCache& textures) {
    Column col{panel_min.x + kPanelPad, panel_min.y + kPanelPad, kPanelW - 2 * kPanelPad};
    col.bottom = &content_bottom_;
    DrawText(ctx, Font::H1, col.Next(kH1Height), kText, "галерея");

    const auto& items = model.gallery_items;

    ReserveBackRow(ctx, panel_min, panel_max, col.w);

    constexpr int kColumns = 4;
    constexpr float kTileGap = 6.0f;
    const float tile_w = (col.w - kTileGap * (kColumns - 1)) / kColumns;
    const float tile_h = tile_w * 0.94f;

    float content_h = 0.0f;
    {
        std::string current;
        int in_row = 0;
        for (const auto& item : items) {
            if (item.date_label != current) {
                current = item.date_label;
                if (in_row != 0) {
                    content_h += tile_h + kTileGap;
                    in_row = 0;
                }
                content_h += 26.0f;
            }
            if (in_row == 0) content_h += tile_h + kTileGap;
            in_row = (in_row + 1) % kColumns;
        }
    }

    const ImVec2 view_min(col.x, col.y);
    const ImVec2 view_max(col.x + col.w, panel_max.y - kPanelPad - 15.0f - kItemGap -
                                             kRowH_TwoLine - kItemGap);

    scroll_.Begin(ctx, HashId("gallery-scroll"), view_min, view_max, content_h);

    float y = view_min.y;
    std::string current_date;
    int column = 0;
    std::uint32_t index = 0;

    for (const auto& item : items) {
        if (item.date_label != current_date) {
            if (column != 0) {
                y += tile_h + kTileGap;
                column = 0;
            }
            current_date = item.date_label;
            DrawText(ctx, Font::Small, ImVec2(col.x, y), kText, current_date.c_str());
            y += 26.0f;
        }

        const ImVec2 pos(col.x + column * (tile_w + kTileGap), y);
        const std::uint32_t id = HashId("g-tile", index++);
        Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + tile_w, pos.y + tile_h));

        const float scale = 1.0f + 0.06f * it.hover - 0.05f * it.press;
        const ImVec2 center(pos.x + tile_w * 0.5f, pos.y + tile_h * 0.5f);
        const ImVec2 half(tile_w * 0.5f * scale, tile_h * 0.5f * scale);
        const ImVec2 a = ctx.At(ImVec2(center.x - half.x, center.y - half.y));
        const ImVec2 b = ctx.At(ImVec2(center.x + half.x, center.y + half.y));

        const float screen_y = pos.y - scroll_.offset();
        const bool on_screen = screen_y + tile_h >= view_min.y && screen_y <= view_max.y;

        ImTextureID tex = on_screen ? textures.Find(item.display_name) : ImTextureID{};
        if (on_screen && !tex && item.thumb_ready && !item.thumbnail.empty()) {
            tex = textures.FromRgba(item.display_name, item.thumbnail.data(),
                                    static_cast<int>(item.thumb_width),
                                    static_cast<int>(item.thumb_height));
            model.gallery->MarkUploaded(item.path);
        } else if (on_screen && !tex && item.thumb_uploaded) {
            model.gallery->ReloadThumbnail(item.path);
        }

        if (tex)
            ctx.dl->AddImageRounded(tex, a, b, ImVec2(0, 0), ImVec2(1, 1),
                                    ctx.Fade(IM_COL32_WHITE), 10.0f);
        else
            ctx.dl->AddRectFilled(a, b, ctx.Fade(IM_COL32(255, 255, 255, 20)), 10.0f);

        if (it.hover > 0.01f)
            ctx.dl->AddRect(a, b, ctx.Fade(IM_COL32(255, 255, 255, static_cast<int>(200 * it.hover))),
                            10.0f, 0, 2.0f);

        DrawThumbBadge(ctx, ImVec2(pos.x + 4.0f, pos.y + 4.0f), item.duration_label.c_str());

        if (it.clicked) OpenInPlayer(model, item.path);

        column = (column + 1) % kColumns;
        if (column == 0) y += tile_h + kTileGap;
    }

    if (items.empty())
        DrawText(ctx, Font::Small, ImVec2(col.x, view_min.y + 8.0f), kTextMuted,
                 "Здесь появятся ваши записи и мгновенные повторы");

    scroll_.End(ctx);

}

}
