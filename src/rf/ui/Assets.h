#pragma once
#include <d3d11.h>
#include <imgui.h>

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/ui/Theme.h"

namespace rf::ui {

class IconSet {
public:
    Status Load(ID3D11Device* device, const std::filesystem::path& dir, float dpi_scale);
    void Release();

    [[nodiscard]] ImTextureID Get(const char* name) const;
    [[nodiscard]] ImVec2 Size(const char* name) const;

private:
    struct Icon {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ImVec2 size{};
    };
    std::unordered_map<std::string, Icon> icons_;
    float dpi_scale_ = 1.0f;
};

class FontSet {
public:
    void Load(ImGuiIO& io, const std::filesystem::path& dir, float dpi_scale);

    [[nodiscard]] ImFont* Get(Font f) const { return fonts_[static_cast<int>(f)]; }
    [[nodiscard]] bool using_design_fonts() const { return design_fonts_; }

private:
    ImFont* fonts_[static_cast<int>(Font::Count)] = {};
    bool design_fonts_ = false;
};

class TextureCache {
public:
    void Init(ID3D11Device* device) { device_ = device; }
    void Release();

    ImTextureID FromRgba(const std::string& key, const std::uint8_t* pixels, int w, int h);
    [[nodiscard]] ImTextureID Find(const std::string& key) const;
    [[nodiscard]] bool Contains(const std::string& key) const { return Find(key) != ImTextureID{}; }

private:

    static constexpr std::size_t kMaxTextures = 192;

    ID3D11Device* device_ = nullptr;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> textures_;
    mutable std::deque<std::string> order_;
};

}
