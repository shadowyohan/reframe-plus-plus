#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rf/core/Status.h"

namespace rf {

struct GalleryItem {
    std::filesystem::path path;
    std::string display_name;
    std::string date_label;
    std::string duration_label;
    std::uint64_t size_bytes = 0;
    std::int64_t sort_key = 0;
    double duration_seconds = 0.0;

    std::vector<std::uint8_t> thumbnail;
    std::uint32_t thumb_width = 0;
    std::uint32_t thumb_height = 0;
    bool thumb_ready = false;
    bool thumb_uploaded = false;
};

class Gallery {
public:
    ~Gallery();

    void Start(const std::filesystem::path& dir);
    void Stop();

    void Refresh();

    void SetDirectory(const std::filesystem::path& dir);

    std::vector<GalleryItem> Snapshot() const;

    void MarkUploaded(const std::filesystem::path& path);

    void ReloadThumbnail(const std::filesystem::path& path);

    void Remove(const std::filesystem::path& path);

    [[nodiscard]] bool scanning() const { return scanning_.load(); }
    [[nodiscard]] std::uint64_t total_bytes() const { return total_bytes_.load(); }

private:
    void Worker();

    std::filesystem::path dir_;
    mutable std::mutex mutex_;
    std::vector<GalleryItem> items_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> rescan_{true};
    std::atomic<bool> scanning_{false};
    std::atomic<std::uint64_t> total_bytes_{0};
};

Status ExtractThumbnail(const std::filesystem::path& file, std::uint32_t max_edge,
                        std::vector<std::uint8_t>& out_bgra, std::uint32_t& out_w,
                        std::uint32_t& out_h, double& out_duration_seconds);

}
