#include "rf/ui/Assets.h"

#include <windows.h>

#include <fstream>
#include <vector>

extern "C" {
#include <nanosvg.h>
#include <nanosvgrast.h>
}

#include "rf/core/Log.h"
#include "rf/core/Strings.h"
#include "rf/ui/EmbeddedAssets.h"

using Microsoft::WRL::ComPtr;

namespace rf::ui {
namespace {

ComPtr<ID3D11ShaderResourceView> UploadRgba(ID3D11Device* device, const std::uint8_t* pixels, int w,
                                            int h) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels;
    data.SysMemPitch = static_cast<UINT>(w * 4);

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srv, &view))) return nullptr;
    return view;
}

void PremultiplyInPlace(std::vector<std::uint8_t>& rgba) {
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        rgba[i + 0] = static_cast<std::uint8_t>(rgba[i + 0] * a / 255);
        rgba[i + 1] = static_cast<std::uint8_t>(rgba[i + 1] * a / 255);
        rgba[i + 2] = static_cast<std::uint8_t>(rgba[i + 2] * a / 255);
    }
}

std::filesystem::path FindFontFile(const std::filesystem::path& asset_dir,
                                   std::initializer_list<const wchar_t*> names) {
    wchar_t windir[MAX_PATH] = {};
    ::GetWindowsDirectoryW(windir, MAX_PATH);
    const std::filesystem::path system_fonts = std::filesystem::path(windir) / L"Fonts";

    for (const wchar_t* name : names) {
        for (const auto& dir : {asset_dir, system_fonts}) {
            std::error_code ec;
            const auto candidate = dir / name;
            if (std::filesystem::exists(candidate, ec)) return candidate;
        }
    }
    return {};
}

}

Status IconSet::Load(ID3D11Device* device, const std::filesystem::path& dir, float dpi_scale) {
    dpi_scale_ = dpi_scale;

    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (!rasterizer) return Status::Fail("nsvgCreateRasterizer failed");

    const float supersample = dpi_scale * 2.0f;
    int loaded = 0;
    bool from_disk = false;

    std::vector<std::pair<std::string, std::string>> sources;

    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".svg") continue;

            std::ifstream file(entry.path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
            if (!text.empty()) sources.emplace_back(entry.path().stem().string(), std::move(text));
        }
        from_disk = !sources.empty();
    }

    if (sources.empty()) {
        for (const std::string& name : assets::List("icons/")) {
            const assets::Blob blob = assets::Find(name);
            if (blob.empty()) continue;
            const auto stem = std::filesystem::path(name).stem().string();
            sources.emplace_back(
                stem, std::string(reinterpret_cast<const char*>(blob.data), blob.size));
        }
    }

    for (auto& [name, text] : sources) {

        NSVGimage* image = nsvgParse(text.data(), "px", 96.0f);
        if (!image || image->width <= 0 || image->height <= 0) {
            RF_WARN("icon {} could not be parsed", name);
            if (image) nsvgDelete(image);
            continue;
        }

        const int w = static_cast<int>(image->width * supersample + 0.5f);
        const int h = static_cast<int>(image->height * supersample + 0.5f);

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4, 0);
        nsvgRasterize(rasterizer, image, 0, 0, supersample, pixels.data(), w, h, w * 4);
        PremultiplyInPlace(pixels);

        Icon icon;
        icon.size = ImVec2(image->width, image->height);
        icon.srv = UploadRgba(device, pixels.data(), w, h);
        nsvgDelete(image);

        if (!icon.srv) {
            RF_WARN("icon {} could not be uploaded", name);
            continue;
        }
        icons_.emplace(name, std::move(icon));
        ++loaded;
    }

    nsvgDeleteRasterizer(rasterizer);
    RF_INFO("loaded {} icons ({}) at {:.2f}x", loaded, from_disk ? "from assets/icons" : "embedded",
            supersample);
    return loaded > 0 ? Status::Ok() : Status::Fail("no icons were loaded");
}

void IconSet::Release() { icons_.clear(); }

ImTextureID IconSet::Get(const char* name) const {
    const auto it = icons_.find(name);
    if (it == icons_.end()) return ImTextureID{};
    return reinterpret_cast<ImTextureID>(it->second.srv.Get());
}

ImVec2 IconSet::Size(const char* name) const {
    const auto it = icons_.find(name);
    return it == icons_.end() ? ImVec2(0, 0) : it->second.size;
}

void FontSet::Load(ImGuiIO& io, const std::filesystem::path& dir, float dpi_scale) {

    const assets::Blob embedded_regular = assets::Find("fonts/Actay-Regular.otf");
    const assets::Blob embedded_wide = assets::Find("fonts/ActayWide-BoldItalic.otf");
    const assets::Blob embedded_wide_bold = assets::Find("fonts/ActayWide-Bold.otf");

    const auto regular = FindFontFile(dir, {L"Actay-Regular.otf", L"Actay-Regular.ttf",
                                            L"ActayRegular.ttf", L"segoeui.ttf"});
    const auto wide = FindFontFile(dir, {L"ActayWide-BoldItalic.otf", L"ActayWide-BoldItalic.ttf",
                                         L"ActayWideBoldItalic.ttf", L"seguibli.ttf"});
    const auto wide_bold = FindFontFile(dir, {L"ActayWide-Bold.otf", L"ActayWide-Bold.ttf",
                                              L"ActayWideBold.ttf", L"seguibl.ttf", L"segoeuib.ttf"});

    design_fonts_ = !embedded_regular.empty() || regular.filename().wstring().starts_with(L"Actay");
    if (!design_fonts_)
        RF_WARN("Actay not embedded and not found in {} - falling back to Segoe UI", dir.string());

    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        builder.AddChar(0x2039);
        builder.AddChar(0x203A);
        builder.AddChar(0x00A9);
        builder.AddChar(0x2014);
        builder.AddChar(0x2026);
        builder.BuildRanges(&ranges);
    }

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;

    auto add = [&](const assets::Blob& blob, const std::filesystem::path& file,
                   float size) -> ImFont* {
        if (!file.empty() && file.filename().wstring().starts_with(L"Actay")) {
            const std::string narrow = ToUtf8(file.wstring());
            if (ImFont* font =
                    io.Fonts->AddFontFromFileTTF(narrow.c_str(), size * dpi_scale, &cfg, ranges.Data))
                return font;
        }
        if (!blob.empty()) {
            ImFontConfig memory_cfg = cfg;

            memory_cfg.FontDataOwnedByAtlas = false;
            if (ImFont* font = io.Fonts->AddFontFromMemoryTTF(
                    const_cast<unsigned char*>(blob.data), static_cast<int>(blob.size),
                    size * dpi_scale, &memory_cfg, ranges.Data))
                return font;
        }
        if (!file.empty()) {
            const std::string narrow = ToUtf8(file.wstring());
            if (ImFont* font =
                    io.Fonts->AddFontFromFileTTF(narrow.c_str(), size * dpi_scale, &cfg, ranges.Data))
                return font;
        }
        return io.Fonts->AddFontDefault();
    };

    fonts_[static_cast<int>(Font::H1)] = add(embedded_wide, wide, 30.0f);
    fonts_[static_cast<int>(Font::H2)] = add(embedded_wide, wide, 24.0f);
    fonts_[static_cast<int>(Font::Body)] = add(embedded_regular, regular, 20.0f);
    fonts_[static_cast<int>(Font::Small)] = add(embedded_regular, regular, 16.0f);
    fonts_[static_cast<int>(Font::Badge)] = add(embedded_wide_bold, wide_bold, 14.0f);
    fonts_[static_cast<int>(Font::BadgeItalic)] = add(embedded_wide, wide, 14.0f);
    fonts_[static_cast<int>(Font::Tiny)] = add(embedded_regular, regular, 13.0f);
    fonts_[static_cast<int>(Font::Toast)] = add(embedded_wide, wide, 16.0f);
    fonts_[static_cast<int>(Font::Warning)] = add(embedded_regular, regular, 17.0f);

    io.Fonts->Build();
}

void TextureCache::Release() {
    textures_.clear();
    order_.clear();
}

ImTextureID TextureCache::FromRgba(const std::string& key, const std::uint8_t* pixels, int w,
                                   int h) {
    if (!device_ || !pixels || w <= 0 || h <= 0) return ImTextureID{};
    auto view = UploadRgba(device_, pixels, w, h);
    if (!view) return ImTextureID{};

    while (order_.size() >= kMaxTextures) {
        textures_.erase(order_.front());
        order_.pop_front();
    }

    ImTextureID id = reinterpret_cast<ImTextureID>(view.Get());
    textures_[key] = std::move(view);
    order_.push_back(key);
    return id;
}

ImTextureID TextureCache::Find(const std::string& key) const {
    const auto it = textures_.find(key);
    if (it == textures_.end()) return ImTextureID{};
    return reinterpret_cast<ImTextureID>(it->second.Get());
}

}
