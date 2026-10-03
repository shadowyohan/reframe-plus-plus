#include "rf/ui/Editor.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "rf/core/Lang.h"
#include "rf/player/TrackMixer.h"
#include "rf/player/VideoPlayer.h"

namespace rf::ui {
namespace {

using namespace theme;

constexpr float kMargin = 25.0f;
constexpr float kPad = 20.0f;
constexpr float kSectionGap = 30.0f;
constexpr float kHeaderH = 70.0f;
constexpr float kControlsH = 56.0f;
constexpr float kTracksPanelW = 534.0f;
constexpr float kTrackRowH = 35.0f;
constexpr float kDialogW = 670.0f;
constexpr float kDialogButtonH = 50.0f;
constexpr float kBarH = 14.0f;
constexpr float kHandleW = 14.0f;
constexpr float kHandleH = 31.0f;
constexpr double kShortestClip = 0.5;
constexpr double kDriftSeconds = 0.08;
constexpr float kVideoFadeStart = 0.164f;
constexpr float kVideoFadeEnd = 0.5f;
constexpr float kTracksSlide = 60.0f;

constexpr ImU32 kEditorPanel = IM_COL32(0x25, 0x25, 0x25, 255);
constexpr ImU32 kDangerSoft = IM_COL32(0xff, 0x7b, 0x7b, 255);
constexpr ImU32 kTracksButton = IM_COL32(0x5e, 0xcf, 0xff, 255);
constexpr ImU32 kSaveButton = IM_COL32(0x63, 0xff, 0x3c, 255);
constexpr ImU32 kTrimFill = IM_COL32(0x9e, 0xff, 0x96, 125);
constexpr ImU32 kTimelineBg = IM_COL32(255, 255, 255, 13);
constexpr ImU32 kTrackRowBg = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kPositiveBg = IM_COL32(0x9e, 0xff, 0x96, 51);
constexpr ImU32 kNeutralBg = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kDialogScrim = IM_COL32(0, 0, 0, 153);

std::string Clock(double seconds) {
    const int total = static_cast<int>(std::max(0.0, seconds));
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

bool IsCoreTrack(std::size_t index, const std::string& name) {
    return index == 0 || name == Tr("Звук системы и микрофон") || name == Tr("Звук системы") ||
           name == Tr("Микрофон");
}

struct Words {
    std::string text;
    ImU32 color;
};

float DrawColoredWords(UiContext& ctx, Font font, ImVec2 pos, float width,
                       const std::vector<Words>& parts, bool draw = true) {
    const float space = MeasureText(ctx, font, " ").x;
    const float line_h = MeasureText(ctx, font, "M").y;
    float x = pos.x, y = pos.y;
    for (const Words& part : parts) {
        std::string_view rest(Tr(std::string_view(part.text)));
        while (!rest.empty()) {
            const std::size_t gap = rest.find(' ');
            const std::string word(rest.substr(0, gap));
            rest = gap == std::string_view::npos ? std::string_view{} : rest.substr(gap + 1);
            if (word.empty()) continue;
            const float word_w = MeasureText(ctx, font, word.c_str()).x;
            if (x > pos.x && x + word_w > pos.x + width) {
                x = pos.x;
                y += line_h;
            }
            if (draw) DrawText(ctx, font, ImVec2(x, y), part.color, word.c_str());
            x += word_w + space;
        }
    }
    return y + line_h - pos.y;
}

bool DialogButton(UiContext& ctx, std::uint32_t id, ImVec2 pos, float width, const char* label,
                  ImU32 background, ImU32 text) {
    const Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + width, pos.y + kDialogButtonH));
    const float grow = 2.0f * it.hover - 2.0f * it.press;
    ctx.dl->AddRectFilled(ctx.At(ImVec2(pos.x - grow, pos.y - grow)),
                          ctx.At(ImVec2(pos.x + width + grow, pos.y + kDialogButtonH + grow)),
                          ctx.Fade(background), 18.0f);
    const ImVec2 size = MeasureText(ctx, Font::Button, label);
    DrawText(ctx, Font::Button,
             ImVec2(pos.x + (width - size.x) * 0.5f, pos.y + (kDialogButtonH - size.y) * 0.5f), text,
             label);
    return it.clicked;
}

bool IconButton(UiContext& ctx, std::uint32_t id, const char* icon, ImVec2 pos, float size, ImU32 tint) {
    const Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + size, pos.y + size));
    DrawIcon(ctx, icon, pos, size, tint, 1.0f + 0.12f * it.hover - 0.1f * it.press);
    return it.clicked;
}

}

void ClipEditor::Open(Hooks hooks, const std::filesystem::path& file, std::vector<ClipMarker> markers) {
    hooks_ = std::move(hooks);
    file_ = file;
    markers_ = std::move(markers);
    trim_start_ = 0.0;
    trim_end_ = 0.0;
    tracks_panel_ = false;
    playing_ = false;
    dialog_ = Dialog::None;
    rise_.Reset(kHiddenOffset);
    rise_.SetTarget(0.0f);
    dialog_in_.Reset(0.0f);
    tracks_in_.Reset(0.0f);
    opened_for_ = 0.0f;

    tracks_.clear();
    const std::vector<std::string> names = ReadAudioTrackNames(file);
    for (std::size_t i = 0; i < names.size(); ++i) {
        TrackRow row;
        row.index = i;
        row.name = names[i].empty() ? TrFormat("Дорожка {}", i + 1) : names[i];
        row.core = IsCoreTrack(i, row.name);
        tracks_.push_back(std::move(row));
    }

    if (hooks_.player) {
        if (hooks_.player->file() != file) (void)hooks_.player->Open(file);
        hooks_.player->Pause();
        hooks_.player->SetVolume(tracks_.empty() ? volume_ : 0.0f);
    }
    if (hooks_.mixer && !tracks_.empty()) {
        hooks_.mixer->Open(file, tracks_.size());
        hooks_.mixer->SetMasterVolume(volume_);
    }
    open_ = true;
}

void ClipEditor::Close() {
    if (!open_) return;
    if (hooks_.mixer) hooks_.mixer->Close();
    if (hooks_.player) {
        hooks_.player->Pause();
        hooks_.player->SetVolume(hooks_.player_volume);
    }
    open_ = false;
    playing_ = false;
    dialog_ = Dialog::None;
    rise_.SetTarget(hide_to_);
}

double ClipEditor::duration() const { return hooks_.player ? hooks_.player->duration() : 0.0; }

bool ClipEditor::dirty() const {
    const double total = duration();
    if (trim_start_ > 0.05 || (total > 0.0 && trim_end_ < total - 0.05)) return true;
    return std::any_of(tracks_.begin(), tracks_.end(), [](const TrackRow& row) {
        return row.removed || std::abs(row.gain - 1.0f) > 0.001f;
    });
}

void ClipEditor::RequestClose() {
    if (dirty())
        dialog_ = Dialog::ConfirmClose;
    else
        Close();
}

void ClipEditor::Escape() {
    if (dialog_ != Dialog::None)
        dialog_ = Dialog::None;
    else
        RequestClose();
}

void ClipEditor::SeekTo(double seconds) {
    if (hooks_.player) hooks_.player->Seek(seconds);
    if (hooks_.mixer) hooks_.mixer->Seek(seconds);
}

void ClipEditor::TogglePlay() {
    if (!hooks_.player) return;
    if (playing_) {
        hooks_.player->Pause();
        if (hooks_.mixer) hooks_.mixer->Pause();
        playing_ = false;
        return;
    }
    double from = hooks_.player->position();
    if (from < trim_start_ || from >= trim_end_ - 0.05) {
        from = trim_start_;
        hooks_.player->Seek(from);
    }
    hooks_.player->Play();
    if (hooks_.mixer) hooks_.mixer->Play(from);
    playing_ = true;
}

void ClipEditor::SyncPlayback() {
    if (!hooks_.player) return;
    hooks_.player->Update();
    const double total = duration();
    if (trim_end_ <= 0.0 && total > 0.0) trim_end_ = total;
    if (!playing_) return;

    const double position = hooks_.player->position();
    if (position >= trim_end_ - 0.01 || !hooks_.player->playing()) {
        TogglePlay();
        SeekTo(trim_start_);
        return;
    }
    if (hooks_.mixer && std::abs(hooks_.mixer->position() - position) > kDriftSeconds)
        hooks_.mixer->Seek(position);
}

void ClipEditor::Save(bool replace_original) {
    ClipEdit edit;
    edit.file = file_;
    edit.replace_original = replace_original;
    edit.trim_start = trim_start_;
    edit.trim_end = trim_end_;
    edit.width = hooks_.player ? hooks_.player->width() : 0;
    edit.height = hooks_.player ? hooks_.player->height() : 0;
    for (const TrackRow& row : tracks_) {
        edit.tracks.push_back({!row.removed, row.gain});
        if (!row.removed) edit.kept_names.push_back(row.name);
    }
    edit.first_kept_is_full_mix =
        !tracks_.empty() && !tracks_.front().removed && FirstAudioTrackIsFullMix(file_);

    const auto start_ms = static_cast<std::int64_t>(trim_start_ * 1000.0);
    const auto end_ms = static_cast<std::int64_t>(trim_end_ * 1000.0);
    for (const ClipMarker& marker : markers_)
        if (marker.ms >= start_ms && marker.ms <= end_ms)
            edit.markers.push_back({static_cast<std::uint32_t>(marker.ms - start_ms), marker.tag});

    const auto on_save = hooks_.on_save;
    Close();
    if (on_save) on_save(edit);
}

void ClipEditor::DrawHeader(UiContext& ctx, ImVec2 min, float width) {
    DrawText(ctx, Font::H1, min, kAppAccent, "Редактор клипа");
    const std::string name = file_.filename().string();
    DrawText(ctx, Font::Body, ImVec2(min.x, min.y + 46.0f), kText, name.c_str());

    const ImVec2 close_pos(min.x + width - 50.0f, min.y);
    if (IconButton(ctx, HashId("ed-close"), "close", close_pos, 50.0f, kText)) RequestClose();

    if (hooks_.is_favorite && hooks_.on_toggle_favorite) {
        const ImVec2 star_pos(close_pos.x - 50.0f, min.y + 9.0f);
        const bool favorite = hooks_.is_favorite();
        if (IconButton(ctx, HashId("ed-star"), "star", star_pos, 32.0f,
                       favorite ? IM_COL32(255, 204, 77, 255) : IM_COL32(255, 255, 255, 90)))
            hooks_.on_toggle_favorite();
    }
}

void ClipEditor::DrawVideo(UiContext& ctx, ImVec2 min, ImVec2 max) {
    const float area_w = max.x - min.x, area_h = max.y - min.y;
    float w = area_w, h = area_w * 9.0f / 16.0f;
    if (h > area_h) {
        h = area_h;
        w = area_h * 16.0f / 9.0f;
    }
    const ImVec2 vmin(min.x + (area_w - w) * 0.5f, min.y + (area_h - h) * 0.5f);
    const ImVec2 vmax(vmin.x + w, vmin.y + h);
    const float saved_alpha = ctx.alpha;
    const float fade = std::clamp((opened_for_ - kVideoFadeStart) / (kVideoFadeEnd - kVideoFadeStart), 0.0f, 1.0f);
    ctx.alpha *= 1.0f - (1.0f - fade) * (1.0f - fade);
    ctx.dl->AddRectFilled(ctx.At(vmin), ctx.At(vmax), ctx.Fade(IM_COL32(0, 0, 0, 255)), 23.0f);

    if (open_ && hooks_.player) {
        if (ImTextureID tex = reinterpret_cast<ImTextureID>(hooks_.player->frame())) {
            const float aspect = static_cast<float>(hooks_.player->width()) /
                                 static_cast<float>(std::max(1u, hooks_.player->height()));
            float iw = w, ih = w / aspect;
            if (ih > h) {
                ih = h;
                iw = h * aspect;
            }
            const ImVec2 c((vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f);
            ctx.dl->AddImageRounded(tex, ctx.At(ImVec2(c.x - iw * 0.5f, c.y - ih * 0.5f)),
                                    ctx.At(ImVec2(c.x + iw * 0.5f, c.y + ih * 0.5f)), ImVec2(0, 0),
                                    ImVec2(1, 1), ctx.Fade(kText), 23.0f);
        }
    }
    ctx.alpha = saved_alpha;
    if (Hit(ctx, HashId("ed-video"), vmin, vmax).clicked) TogglePlay();
}

void ClipEditor::DrawTracksPanel(UiContext& ctx, ImVec2 min, ImVec2 max) {
    ctx.dl->AddRectFilled(ctx.At(min), ctx.At(max), ctx.Fade(kEditorPanel), 38.0f);
    const float x = min.x + kPad;
    const float w = max.x - min.x - 2 * kPad;
    float y = min.y + kPad;

    DrawText(ctx, Font::H1, ImVec2(x, y), kAppAccent, "Управление дорожками");
    if (IconButton(ctx, HashId("ed-tracks-close"), "close", ImVec2(x + w - 40.0f, y - 2.0f), 40.0f,
                   kDangerSoft))
        tracks_panel_ = false;
    y += 40.0f + 15.0f;

    constexpr float kIcon = 25.0f, kSlider = 122.0f;
    const float name_w = w - 20.0f - kIcon - 5.0f - kSlider - 10.0f - kIcon - 20.0f;
    bool any = false;
    for (TrackRow& row : tracks_) {
        if (row.removed) continue;
        any = true;
        const ImVec2 name_size = MeasureTextWrapped(ctx, Font::Label, name_w, row.name.c_str());
        const float row_h = std::max(kTrackRowH, name_size.y + 10.0f);
        const ImVec2 rmin(x, y), rmax(x + w, y + row_h);
        ctx.dl->AddRectFilled(ctx.At(rmin), ctx.At(rmax), ctx.Fade(kTrackRowBg), 12.0f);
        DrawTextWrapped(ctx, Font::Label, ImVec2(x + 10.0f, y + (row_h - name_size.y) * 0.5f), name_w,
                        row.core ? kAccent : kText, row.name.c_str());

        const float cy = y + row_h * 0.5f;
        float cx = x + w - 10.0f - kIcon;
        if (IconButton(ctx, HashId("ed-track-trash", static_cast<std::uint32_t>(row.index)), "trash",
                       ImVec2(cx, cy - kIcon * 0.5f), kIcon, kDangerSoft)) {
            row.removed = true;
            if (hooks_.mixer) hooks_.mixer->SetGain(row.index, 0.0f);
        }
        cx -= 10.0f + kSlider;
        float percent = row.gain * 100.0f;
        if (SliderBar(ctx, HashId("ed-track-gain", static_cast<std::uint32_t>(row.index)),
                      ImVec2(cx, cy - 7.5f), kSlider, 15.0f, percent, 0.0f, TrackMixer::kMaxGain * 100.0f)) {
            row.gain = std::round(percent) / 100.0f;
            if (hooks_.mixer) hooks_.mixer->SetGain(row.index, row.gain);
        }
        cx -= 5.0f + kIcon;
        if (IconButton(ctx, HashId("ed-track-mute", static_cast<std::uint32_t>(row.index)),
                       row.gain > 0.001f ? "volume-high" : "volume-off", ImVec2(cx, cy - kIcon * 0.5f),
                       kIcon, kText)) {
            if (row.gain > 0.001f) {
                row.gain_before_mute = row.gain;
                row.gain = 0.0f;
            } else {
                row.gain = row.gain_before_mute > 0.001f ? row.gain_before_mute : 1.0f;
            }
            if (hooks_.mixer) hooks_.mixer->SetGain(row.index, row.gain);
        }
        y += row_h + 10.0f;
    }
    if (!any) DrawText(ctx, Font::Body, ImVec2(x, y), kTextMuted, "В клипе не осталось звука");
}

void ClipEditor::DrawTimeline(UiContext& ctx, ImVec2 min, ImVec2 max) {
    ctx.dl->AddRectFilled(ctx.At(min), ctx.At(max), ctx.Fade(kTimelineBg), 22.0f);
    const double total = duration();
    if (total <= 0.0) return;

    const float bar_x = min.x + 10.0f + kHandleW * 0.5f;
    const float bar_w = (max.x - min.x) - 20.0f - kHandleW;
    const float bar_y = max.y - 9.0f - kBarH;
    const float center_y = bar_y + kBarH * 0.5f;
    const auto x_of = [&](double t) { return bar_x + bar_w * static_cast<float>(t / total); };
    const auto time_at = [&](float x) {
        return std::clamp(static_cast<double>((x - ctx.offset.x - bar_x) / bar_w) * total, 0.0, total);
    };

    const float start_x = x_of(trim_start_);
    const float end_x = x_of(trim_end_);
    const ImVec2 handle_half(kHandleW, kHandleH * 0.5f + 4.0f);
    const std::uint32_t start_id = HashId("ed-trim-start");
    const std::uint32_t end_id = HashId("ed-trim-end");
    const std::uint32_t bar_id = HashId("ed-bar");

    Hit(ctx, start_id, ImVec2(start_x - handle_half.x, center_y - handle_half.y),
        ImVec2(start_x + handle_half.x, center_y + handle_half.y), false);
    Hit(ctx, end_id, ImVec2(end_x - handle_half.x, center_y - handle_half.y),
        ImVec2(end_x + handle_half.x, center_y + handle_half.y), false);
    ctx.Reserve(ctx.At(ImVec2(start_x - handle_half.x, center_y - handle_half.y)),
                ctx.At(ImVec2(start_x + handle_half.x, center_y + handle_half.y)));
    ctx.Reserve(ctx.At(ImVec2(end_x - handle_half.x, center_y - handle_half.y)),
                ctx.At(ImVec2(end_x + handle_half.x, center_y + handle_half.y)));
    Hit(ctx, bar_id, ImVec2(bar_x - 6.0f, bar_y - 8.0f), ImVec2(bar_x + bar_w + 6.0f, bar_y + kBarH + 8.0f));

    if (ctx.mouse_down && ctx.active_id == start_id) {
        trim_start_ = std::clamp(time_at(ctx.mouse.x), 0.0, trim_end_ - kShortestClip);
        if (hooks_.player && hooks_.player->position() < trim_start_) SeekTo(trim_start_);
    } else if (ctx.mouse_down && ctx.active_id == end_id) {
        trim_end_ = std::clamp(time_at(ctx.mouse.x), trim_start_ + kShortestClip, total);
        if (hooks_.player && hooks_.player->position() > trim_end_) SeekTo(trim_start_);
    } else if (ctx.mouse_down && ctx.active_id == bar_id) {
        SeekTo(std::clamp(time_at(ctx.mouse.x), trim_start_, trim_end_));
    }

    ctx.dl->AddRectFilled(ctx.At(ImVec2(bar_x, bar_y)), ctx.At(ImVec2(bar_x + bar_w, bar_y + kBarH)),
                          ctx.Fade(kTrack), kBarH * 0.5f);
    const float sel_min = x_of(trim_start_), sel_max = x_of(trim_end_);
    ctx.dl->AddRectFilled(ctx.At(ImVec2(sel_min, bar_y)), ctx.At(ImVec2(sel_max, bar_y + kBarH)),
                          ctx.Fade(kTrimFill), kBarH * 0.5f);
    for (float dot = sel_min + 12.0f; dot < sel_max - 8.0f; dot += 9.0f)
        ctx.dl->AddCircleFilled(ctx.At(ImVec2(dot, center_y)), 2.5f, ctx.Fade(kAccent), 8);
    for (const float hx : {sel_min, sel_max})
        ctx.dl->AddRectFilled(ctx.At(ImVec2(hx - kHandleW * 0.5f, center_y - kHandleH * 0.5f)),
                              ctx.At(ImVec2(hx + kHandleW * 0.5f, center_y + kHandleH * 0.5f)),
                              ctx.Fade(kAccent), kHandleW * 0.5f);

    if (hooks_.player) {
        const float px = x_of(hooks_.player->position());
        ctx.dl->AddRectFilled(ctx.At(ImVec2(px - 2.2f, center_y - 17.0f)),
                              ctx.At(ImVec2(px + 2.2f, center_y + 17.0f)), ctx.Fade(kText), 2.2f);
    }

    if (const auto moment = DrawMomentFlags(ctx, markers_, ImVec2(bar_x, bar_y), bar_w, kBarH, total))
        SeekTo(std::clamp(*moment, trim_start_, trim_end_));
}

void ClipEditor::DrawControls(UiContext& ctx, ImVec2 min, float width) {
    constexpr float kBtn = 40.0f, kGap = 20.0f, kVolSlider = 155.0f, kSmall = 35.0f;
    const float center_y = min.y + kControlsH * 0.5f;
    float x = min.x;

    const Interaction play = Hit(ctx, HashId("ed-play"), ImVec2(x, center_y - kBtn * 0.5f),
                                 ImVec2(x + kBtn, center_y + kBtn * 0.5f));
    const float scale = 1.0f + 0.12f * play.hover - 0.1f * play.press;
    if (playing_) {
        const ImVec2 c(x + kBtn * 0.5f, center_y);
        ctx.dl->AddCircleFilled(ctx.At(c), kBtn * 0.5f * scale, ctx.Fade(kText), 32);
        const float bw = 3.5f * scale, bh = 9.0f * scale, bx = 4.0f * scale;
        for (const float side : {-1.0f, 1.0f})
            ctx.dl->AddRectFilled(ctx.At(ImVec2(c.x + side * bx - bw, c.y - bh)),
                                  ctx.At(ImVec2(c.x + side * bx + bw, c.y + bh)), ctx.Fade(kPanelBg), 1.5f);
    } else {
        DrawIcon(ctx, "play", ImVec2(x, center_y - kBtn * 0.5f), kBtn, kText, scale);
    }
    if (play.clicked) TogglePlay();
    x += kBtn + kGap;

    const double position = hooks_.player ? hooks_.player->position() : 0.0;
    const std::string time = std::format("{} / {}", Clock(position - trim_start_),
                                         Clock(trim_end_ - trim_start_));
    const float time_w = MeasureText(ctx, Font::Body, "00:00 / 00:00").x;
    const float right_w = kGap + time_w + kGap + kBtn + 10.0f + kVolSlider + kGap + kSmall + 10.0f + kSmall;
    const float timeline_w = std::max(120.0f, min.x + width - x - right_w);
    DrawTimeline(ctx, ImVec2(x, min.y), ImVec2(x + timeline_w, min.y + kControlsH));
    x += timeline_w + kGap;

    DrawText(ctx, Font::Body, ImVec2(x, center_y - 12.0f), kText, time.c_str());
    x += time_w + kGap;

    if (IconButton(ctx, HashId("ed-mute"), volume_ > 0.001f ? "volume-high" : "volume-off",
                   ImVec2(x, center_y - kBtn * 0.5f), kBtn, kText)) {
        volume_ = volume_ > 0.001f ? 0.0f : 1.0f;
        if (hooks_.mixer) hooks_.mixer->SetMasterVolume(volume_);
    }
    x += kBtn + 10.0f;
    float percent = volume_ * 100.0f;
    if (SliderBar(ctx, HashId("ed-volume"), ImVec2(x, center_y - 11.0f), kVolSlider, 22.0f, percent,
                  0.0f, 100.0f)) {
        volume_ = percent / 100.0f;
        if (hooks_.mixer) hooks_.mixer->SetMasterVolume(volume_);
    }
    x += kVolSlider + kGap;

    if (!tracks_.empty() &&
        IconButton(ctx, HashId("ed-tracks"), "audio-tracks", ImVec2(x, center_y - kSmall * 0.5f), kSmall,
                   kTracksButton))
        tracks_panel_ = !tracks_panel_;
    x += kSmall + 10.0f;
    if (IconButton(ctx, HashId("ed-save"), "edit", ImVec2(x, center_y - kSmall * 0.5f), kSmall, kSaveButton))
        dialog_ = Dialog::ChooseSave;
}

void ClipEditor::DrawDialog(UiContext& ctx, ImVec2 screen, std::vector<ImVec4>& panels) {
    const float shown = std::clamp(dialog_in_.value(), 0.0f, 1.2f);
    const float saved_alpha = ctx.alpha;
    ctx.alpha *= std::clamp(shown, 0.0f, 1.0f);
    ctx.dl->AddRectFilled(ctx.At(ImVec2(kMargin, kMargin)),
                          ctx.At(ImVec2(screen.x - kMargin, screen.y - kMargin)), ctx.Fade(kDialogScrim),
                          42.0f);
    const int first_vertex = ctx.dl->VtxBuffer.Size;

    const bool confirm = dialog_ == Dialog::ConfirmClose;
    const float inner_w = kDialogW - 2 * kPad;
    const std::vector<Words> text =
        confirm ? std::vector<Words>{{"Вы закрываете редактор с изменениями.", kDangerSoft},
                                     {"Закрытие без сохранения приведет к потере ваших изменений", kText}}
                : std::vector<Words>{{"Заменить исходный файл или сохранить изменения в новый?", kText}};

    ImVec2 pos((screen.x - kDialogW) * 0.5f, 0.0f);
    const float text_h = DrawColoredWords(ctx, Font::Body, ImVec2(0.0f, 0.0f), inner_w, text, false);
    const float height = kPad + 40.0f + 15.0f + text_h + 15.0f + kDialogButtonH + kPad;
    pos.y = (screen.y - height) * 0.5f;
    const ImVec2 card_max(pos.x + kDialogW, pos.y + height);
    panels.emplace_back(pos.x, pos.y, card_max.x, card_max.y);

    ctx.dl->AddRectFilled(ctx.At(ImVec2(pos.x - 6.0f, pos.y - 6.0f)), ctx.At(ImVec2(card_max.x + 6.0f, card_max.y + 6.0f)),
                          ctx.Fade(IM_COL32(0, 0, 0, 90)), 44.0f);
    ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(card_max), ctx.Fade(kPanelBg), 38.0f);

    const float x = pos.x + kPad;
    float y = pos.y + kPad;
    DrawText(ctx, Font::H1, ImVec2(x, y), confirm ? kDangerSoft : kAppAccent,
             confirm ? "Вы уверены?" : "Сохранить клип");
    if (IconButton(ctx, HashId("ed-dialog-close"), "close", ImVec2(x + inner_w - 40.0f, y - 2.0f), 40.0f,
                   kDangerSoft))
        dialog_ = Dialog::None;
    y += 40.0f + 15.0f;
    y += DrawColoredWords(ctx, Font::Body, ImVec2(x, y), inner_w, text) + 15.0f;

    const float button_w = (inner_w - 10.0f) * 0.5f;
    if (confirm) {
        if (DialogButton(ctx, HashId("ed-dialog-save"), ImVec2(x, y), button_w, Tr("Сохранить"), kPositiveBg,
                         kAccent))
            dialog_ = Dialog::ChooseSave;
        if (DialogButton(ctx, HashId("ed-dialog-discard"), ImVec2(x + button_w + 10.0f, y), button_w,
                         Tr("Не сохранять"), kNeutralBg, IM_COL32(255, 255, 255, 102)))
            Close();
    } else {
        if (DialogButton(ctx, HashId("ed-dialog-replace"), ImVec2(x, y), button_w, Tr("Заменить"),
                         kPositiveBg, kAccent))
            Save(true);
        else if (DialogButton(ctx, HashId("ed-dialog-copy"), ImVec2(x + button_w + 10.0f, y), button_w,
                              Tr("Сохранить копию"), kNeutralBg, kText))
            Save(false);
    }

    const float scale = 0.92f + 0.08f * shown;
    const ImVec2 pivot(ctx.At(ImVec2((pos.x + card_max.x) * 0.5f, (pos.y + card_max.y) * 0.5f)));
    for (int i = first_vertex; i < ctx.dl->VtxBuffer.Size; ++i) {
        ImVec2& p = ctx.dl->VtxBuffer[i].pos;
        p = ImVec2(pivot.x + (p.x - pivot.x) * scale, pivot.y + (p.y - pivot.y) * scale);
    }
    ctx.alpha = saved_alpha;
}

void ClipEditor::Draw(UiContext& ctx, ImVec2 screen, std::vector<ImVec4>& panels) {
    if (!visible()) return;
    hide_to_ = std::max(kHiddenOffset, screen.y);
    if (!open_) rise_.SetTarget(hide_to_);
    if (open_) SyncPlayback();
    opened_for_ += ctx.dt;
    const float rise = rise_.Update(ctx.dt, open_ ? spring::kEditorRise : spring::kExit);
    dialog_in_.SetTarget(dialog_ != Dialog::None ? 1.0f : 0.0f);
    dialog_in_.Update(ctx.dt, spring::kQuick);
    tracks_in_.SetTarget(tracks_panel_ ? 1.0f : 0.0f);
    const float tracks_shown = std::clamp(tracks_in_.Update(ctx.dt, spring::kMenu), 0.0f, 1.0f);

    const ImVec2 saved_offset = ctx.offset;
    ctx.offset = ImVec2(saved_offset.x, saved_offset.y + std::round(rise));

    const ImVec2 min(kMargin, kMargin);
    const ImVec2 max(screen.x - kMargin, screen.y - kMargin);
    DrawPanel(ctx, min, max, kPanelRadius);
    if (open_ && dialog_ == Dialog::None) panels.emplace_back(min.x, min.y, max.x, max.y);

    const bool was_interactive = ctx.interactive;
    if (dialog_ != Dialog::None || !open_ || rise > 40.0f) ctx.interactive = false;

    const float x = min.x + kPad;
    const float width = max.x - min.x - 2 * kPad;
    DrawHeader(ctx, ImVec2(x, min.y + kPad), width);

    const float controls_y = max.y - kPad - kControlsH;
    const float body_top = min.y + kPad + kHeaderH + kSectionGap;
    const float body_bottom = controls_y - kSectionGap;
    float video_right = x + width;
    if (tracks_shown > 0.01f) {
        const float panel_w = kTracksPanelW * tracks_shown;
        const float slide = kTracksSlide * (1.0f - tracks_shown);
        const ImVec2 panel_min(x + width - kTracksPanelW + slide, body_top);
        const float saved_alpha = ctx.alpha;
        ctx.alpha *= tracks_shown;
        ctx.dl->PushClipRect(ctx.At(ImVec2(x + width - panel_w, body_top)), ctx.At(ImVec2(x + width, body_bottom)), true);
        DrawTracksPanel(ctx, panel_min, ImVec2(panel_min.x + kTracksPanelW, body_bottom));
        ctx.dl->PopClipRect();
        ctx.alpha = saved_alpha;
        video_right = x + width - panel_w - kSectionGap * tracks_shown;
    }
    DrawVideo(ctx, ImVec2(x, body_top), ImVec2(video_right, body_bottom));
    DrawControls(ctx, ImVec2(x, controls_y), width);

    ctx.interactive = was_interactive;
    if (open_ && (dialog_ != Dialog::None || dialog_in_.value() > 0.01f)) {
        if (dialog_ == Dialog::None) ctx.interactive = false;
        DrawDialog(ctx, screen, panels);
        ctx.interactive = was_interactive;
    }
    ctx.offset = saved_offset;
}

}
