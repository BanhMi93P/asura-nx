#include "asura_startup_overlay.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <queue>
#include <system_error>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "renut_engine/gpu/overlay/overlay_drawer.h"

#if defined(__GNUC__)
__asm__(
    ".section .rodata\n"
    ".balign 16\n"
    ".global g_installing_jpg_start\n"
    "g_installing_jpg_start:\n"
    ".incbin \"g:/!Project/!port/Asuras Wrath/temp/repos/renut_nx_ultimate_base/app/assets/installing.jpg\"\n"
    ".global g_installing_jpg_end\n"
    "g_installing_jpg_end:\n"
    ".balign 16\n"
    ".global g_intro_anim_start\n"
    "g_intro_anim_start:\n"
    ".incbin \"g:/!Project/!port/Asuras Wrath/temp/repos/renut_nx_ultimate_base/app/assets/intro_anim.bin\"\n"
    ".global g_intro_anim_end\n"
    "g_intro_anim_end:\n"
    ".previous\n"
);
extern "C" {
extern const uint8_t g_installing_jpg_start[];
extern const uint8_t g_installing_jpg_end[];
extern const uint8_t g_intro_anim_start[];
extern const uint8_t g_intro_anim_end[];
}
#else
static const uint8_t g_installing_jpg_start[1] = {0};
static const uint8_t* const g_installing_jpg_end = g_installing_jpg_start;
static const uint8_t g_intro_anim_start[1] = {0};
static const uint8_t* const g_intro_anim_end = g_intro_anim_start;
#endif

namespace asura {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kAsuraTitleId = 0x43430817u;
constexpr int kRequiredBPresses = 20;
constexpr const char kProfileFolder[] = "0000000000000000";
constexpr const char kTitleFolder[] = "43430817";
constexpr const char kContentTypeFolder[] = "00000002";

struct AwifHeader {
  char magic[4];
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  uint32_t total_duration_ms;
};
static_assert(sizeof(AwifHeader) == 24);

struct AwifFrameEntry {
  uint32_t offset;
  uint32_t size;
  uint32_t duration_ms;
};
static_assert(sizeof(AwifFrameEntry) == 12);

fs::path SanitizePath(fs::path p) {
#if defined(__SWITCH__)
  std::string s = rex::path_to_utf8(p);
  if (s.rfind("sdmc:/", 0) == 0) {
    p = rex::to_path(s.substr(5));
  }
#endif
  return p;
}

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool ProbeStfsPackage(const fs::path& path, uint32_t& out_title_id, uint32_t& out_content_type,
                      std::string& out_display_utf8, std::u16string& out_display_u16) {
  FILE* f = rex::filesystem::OpenFile(path, "rb");
  if (!f) {
    return false;
  }
  std::array<uint8_t, 0x520> hdr{};
  const size_t n = std::fread(hdr.data(), 1, hdr.size(), f);
  std::fclose(f);
  if (n < 0x500) {
    return false;
  }
  const std::string_view magic(reinterpret_cast<const char*>(hdr.data()), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") {
    return false;
  }
  out_content_type = LoadBe32(hdr.data() + 0x344);
  out_title_id = LoadBe32(hdr.data() + 0x360);
  out_display_u16.clear();
  out_display_utf8.clear();
  for (size_t i = 0; i < 64; ++i) {
    const uint8_t* c = hdr.data() + 0x411 + i * 2;
    const uint16_t ch = uint16_t((c[0] << 8) | c[1]);
    if (ch == 0) {
      break;
    }
    out_display_u16.push_back(char16_t(ch));
  }
  if (!out_display_u16.empty()) {
    out_display_utf8 = rex::string::to_utf8(out_display_u16);
  }
  return true;
}

bool WriteContentHeaderFileDirect(const fs::path& user_data_root,
                                  const rex::system::xam::XCONTENT_AGGREGATE_DATA& data,
                                  uint32_t license_mask) {
  const fs::path header_dir =
      user_data_root / kProfileFolder / kTitleFolder / "Headers" / kContentTypeFolder;
  std::error_code ec;
  fs::create_directories(header_dir, ec);
  if (ec) {
    return false;
  }
  const fs::path header_path = header_dir / (data.file_name() + ".header");
  FILE* file = rex::filesystem::OpenFile(header_path, "wb");
  if (!file) {
    return false;
  }
  bool ok = std::fwrite(&data, 1, sizeof(data), file) == sizeof(data);
  if (ok && license_mask != 0) {
    ok = std::fwrite(&license_mask, 1, sizeof(license_mask), file) == sizeof(license_mask);
  }
  if (std::fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

}  // namespace

AsuraStartupDialog::AsuraStartupDialog(rex::ui::ImGuiDrawer* drawer,
                                       std::function<rex::ui::ImmediateDrawer*()> immediate_drawer)
    : rex::ui::ImGuiDialog(drawer), immediate_drawer_(std::move(immediate_drawer)) {}

AsuraStartupDialog::~AsuraStartupDialog() {
  Stop();
}

void AsuraStartupDialog::Stop() {
  stop_requested_.store(true, std::memory_order_release);
  intro_stop_.store(true, std::memory_order_release);
  if (installer_thread_.joinable()) {
    installer_thread_.join();
  }
  if (intro_decode_thread_.joinable()) {
    intro_decode_thread_.join();
  }
  installing_tex_.reset();
  intro_tex_.reset();
  {
    std::lock_guard lock(frame_mtx_);
    ready_rgba_.clear();
    ready_rgba_.shrink_to_fit();
  }
  if (stage_ == Stage::kInstalling || stage_ == Stage::kIntro) {
    rex::ui::SetGameInputBlocked(false);
    stage_ = Stage::kDone;
  }
}

void AsuraStartupDialog::Begin(const rex::PathConfig& defaults,
                               std::function<void(rex::PathConfig)> on_done) {
  Stop();
  stop_requested_.store(false, std::memory_order_release);
  install_finished_.store(false, std::memory_order_release);
  intro_stop_.store(false, std::memory_order_release);

  paths_ = defaults;
  paths_.user_data_root = SanitizePath(paths_.user_data_root);
  on_done_ = std::move(on_done);

  rex::ui::SetGameInputBlocked(true);

  ScanQueuedPackages();

  if (!queue_.empty()) {
    stage_ = Stage::kInstalling;
    bytes_done_.store(0, std::memory_order_release);
    bytes_total_.store(1, std::memory_order_release);
    pkg_index_.store(0, std::memory_order_release);
    pkg_count_.store(int(queue_.size()), std::memory_order_release);
    installer_thread_ = std::thread([this] { InstallWorker(); });
  } else {
    EnterIntroStage();
  }
}

void AsuraStartupDialog::ScanQueuedPackages() {
  queue_.clear();
  scanned_dirs_.clear();

  std::error_code ec;
  const fs::path exe_dir = SanitizePath(rex::filesystem::GetExecutableFolder());
  const fs::path user_dir = paths_.user_data_root.empty() ? exe_dir : paths_.user_data_root;

  // Ensure default Update and dlc directories exist next to executable for release users.
  fs::create_directories(exe_dir / "Update", ec);
  ec.clear();
  fs::create_directories(exe_dir / "dlc", ec);
  ec.clear();

  struct Candidate {
    fs::path dir;
    bool is_update;
  };
  std::vector<Candidate> candidates = {
      {exe_dir / "Update", true},
      {exe_dir / "update", true},
      {exe_dir / "dlc", false},
      {exe_dir / "DLC", false},
  };
  if (user_dir != exe_dir) {
    candidates.push_back({user_dir / "Update", true});
    candidates.push_back({user_dir / "dlc", false});
  }
#if defined(__SWITCH__)
  candidates.push_back({fs::path("/switch/asura-nx/Update"), true});
  candidates.push_back({fs::path("/switch/asura-nx/dlc"), false});
#endif

  auto normalize_key = [](const fs::path& p) {
    std::error_code kec;
    fs::path c = fs::weakly_canonical(p, kec);
    std::string s = rex::path_to_utf8(kec ? p.lexically_normal() : c);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
      s.pop_back();
    }
    return s;
  };

  std::vector<std::string> seen_dirs;
  std::vector<QueuedPackage> update_items;
  std::vector<QueuedPackage> dlc_items;

  for (const auto& cand : candidates) {
    if (!fs::is_directory(cand.dir, ec)) {
      ec.clear();
      continue;
    }
    const std::string key = normalize_key(cand.dir);
    if (std::find(seen_dirs.begin(), seen_dirs.end(), key) != seen_dirs.end()) {
      continue;
    }
    seen_dirs.push_back(key);
    scanned_dirs_.push_back(cand.dir);

    for (fs::recursive_directory_iterator it(cand.dir, ec), end; it != end && !ec; it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        ec.clear();
        continue;
      }
      const fs::path p = it->path();
      const std::string fname = rex::path_to_utf8(p.filename());
      if (fname.empty() || fname[0] == '.' || fname == "installed.txt") {
        continue;
      }
      const uintmax_t fsize = fs::file_size(p, ec);
      if (ec || fsize == 0) {
        ec.clear();
        continue;
      }

      QueuedPackage item;
      item.src_path = p;
      item.rel_path = fs::relative(p, cand.dir, ec);
      if (ec || item.rel_path.empty()) {
        ec.clear();
        item.rel_path = p.filename();
      }
      item.filename = fname;
      item.from_update_dir = cand.is_update;
      item.display_name = fname;
      item.display_name_u16 = rex::path_to_utf16(p.filename());
      item.payload_bytes = uint64_t(fsize);

      if (cand.is_update) {
        update_items.push_back(std::move(item));
      } else {
        dlc_items.push_back(std::move(item));
      }
    }
    ec.clear();
  }

  auto by_name = [](const QueuedPackage& a, const QueuedPackage& b) {
    return a.filename < b.filename;
  };
  std::sort(update_items.begin(), update_items.end(), by_name);
  std::sort(dlc_items.begin(), dlc_items.end(), by_name);

  queue_.reserve(update_items.size() + dlc_items.size());
  for (auto& u : update_items) {
    queue_.push_back(std::move(u));
  }
  for (auto& d : dlc_items) {
    queue_.push_back(std::move(d));
  }
}

void AsuraStartupDialog::InstallWorker() {
  const fs::path exe_dir = SanitizePath(rex::filesystem::GetExecutableFolder());
  const fs::path user_root = paths_.user_data_root.empty() ? exe_dir : paths_.user_data_root;

  {
    std::lock_guard lock(status_mtx_);
    status_title_ = "Scanning Updates & DLC...";
    status_detail_ = "Inspecting packages before extraction...";
  }

  // Pass 1: measure total payload bytes across all queued items for exact % progress.
  uint64_t total_bytes = 0;
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (stop_requested_.load(std::memory_order_acquire)) {
      return;
    }
    QueuedPackage& pkg = queue_[i];
    pkg.is_stfs = ProbeStfsPackage(pkg.src_path, pkg.title_id, pkg.content_type,
                                   pkg.display_name, pkg.display_name_u16);
    if (pkg.display_name.empty()) {
      pkg.display_name = pkg.filename;
    }
    if (pkg.display_name_u16.empty()) {
      pkg.display_name_u16 = rex::path_to_utf16(pkg.src_path.filename());
    }

    if (pkg.is_stfs) {
      if (pkg.title_id != 0 && pkg.title_id != kAsuraTitleId) {
        REXLOG_WARN("[Installer] Skipping {} ({}) for foreign title {:08X}",
                    pkg.filename, pkg.display_name, pkg.title_id);
        pkg.payload_bytes = 0;
        continue;
      }
      auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", pkg.src_path);
      if (device->Initialize()) {
        uint64_t pkg_sum = 0;
        if (auto* root = device->ResolvePath("")) {
          std::queue<rex::filesystem::Entry*> q;
          q.push(root);
          while (!q.empty()) {
            auto* e = q.front();
            q.pop();
            for (auto& c : e->children()) {
              q.push(c.get());
            }
            if ((e->attributes() & rex::filesystem::kFileAttributeDirectory) == 0) {
              pkg_sum += uint64_t(e->size());
            }
          }
        }
        pkg.payload_bytes = std::max<uint64_t>(pkg_sum, 1);
      } else {
        REXLOG_ERROR("[Installer] Failed to mount STFS package {}", pkg.filename);
        pkg.payload_bytes = 0;
      }
    } else if (!pkg.from_update_dir) {
      REXLOG_WARN("[Installer] Ignoring non-STFS file in dlc folder: {}", pkg.filename);
      pkg.payload_bytes = 0;
    }
    total_bytes += pkg.payload_bytes;
  }

  bytes_total_.store(std::max<uint64_t>(total_bytes, 1), std::memory_order_release);

  constexpr size_t kChunkSize = 2 * 1024 * 1024;
  std::vector<uint8_t> copy_buf(kChunkSize);
  std::error_code ec;

  // Pass 2: extract/copy with live byte-accurate progress, then delete raw source files.
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (stop_requested_.load(std::memory_order_acquire)) {
      return;
    }
    const QueuedPackage& pkg = queue_[i];
    pkg_index_.store(int(i + 1), std::memory_order_release);

    if (pkg.payload_bytes == 0) {
      continue;
    }

    {
      std::lock_guard lock(status_mtx_);
      status_title_ = pkg.from_update_dir ? "Installing Game Update..." : "Installing DLC Content...";
      status_detail_ = fmt::format("[{}/{}] Preparing {}...", i + 1, queue_.size(), pkg.display_name);
    }

    bool pkg_ok = true;

    if (pkg.is_stfs) {
      auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", pkg.src_path);
      if (!device->Initialize()) {
        REXLOG_ERROR("[Installer] Failed to initialize STFS {}", pkg.filename);
        continue;
      }

      const fs::path install_path =
          user_root / kProfileFolder / kTitleFolder / kContentTypeFolder / pkg.filename;
      fs::create_directories(install_path, ec);
      if (ec) {
        REXLOG_ERROR("[Installer] Failed to create install dir {}: {}",
                     rex::path_to_utf8(install_path), ec.message());
        ec.clear();
        continue;
      }

      auto* root = device->ResolvePath("");
      if (!root) {
        REXLOG_ERROR("[Installer] Missing root entry in {}", pkg.filename);
        continue;
      }

      std::queue<rex::filesystem::Entry*> q;
      q.push(root);
      while (!q.empty() && pkg_ok) {
        if (stop_requested_.load(std::memory_order_acquire)) {
          return;
        }
        auto* entry = q.front();
        q.pop();
        for (auto& child : entry->children()) {
          q.push(child.get());
        }

        const fs::path dest_path =
            install_path / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));
        if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
          fs::create_directories(dest_path, ec);
          if (ec) {
            pkg_ok = false;
            ec.clear();
          }
          continue;
        }

        fs::create_directories(dest_path.parent_path(), ec);
        ec.clear();

        rex::filesystem::File* in_file = nullptr;
        if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file) != 0 || !in_file) {
          pkg_ok = false;
          break;
        }
        FILE* out_file = rex::filesystem::OpenFile(dest_path, "wb");
        if (!out_file) {
          in_file->Destroy();
          pkg_ok = false;
          break;
        }

        size_t remaining = entry->size();
        size_t offset = 0;
        const std::string entry_name = entry->name();
        while (remaining > 0) {
          if (stop_requested_.load(std::memory_order_acquire)) {
            std::fclose(out_file);
            in_file->Destroy();
            return;
          }
          const size_t to_read = std::min(remaining, kChunkSize);
          size_t bytes_read = 0;
          in_file->ReadSync(std::span<uint8_t>(copy_buf.data(), to_read), offset, &bytes_read);
          if (bytes_read == 0) {
            pkg_ok = false;
            break;
          }
          if (std::fwrite(copy_buf.data(), 1, bytes_read, out_file) != bytes_read) {
            pkg_ok = false;
            break;
          }
          offset += bytes_read;
          remaining -= bytes_read;

          const uint64_t done =
              bytes_done_.fetch_add(uint64_t(bytes_read), std::memory_order_relaxed) +
              uint64_t(bytes_read);
          const double done_mb = double(done) / (1024.0 * 1024.0);
          const double tot_mb = double(bytes_total_.load(std::memory_order_relaxed)) / (1024.0 * 1024.0);
          std::lock_guard lock(status_mtx_);
          status_detail_ = fmt::format("[{}/{}] {} - {} ({:.1f} / {:.1f} MB)", i + 1,
                                       queue_.size(), pkg.display_name, entry_name, done_mb, tot_mb);
        }
        if (std::fclose(out_file) != 0) {
          pkg_ok = false;
        }
        in_file->Destroy();
      }

      uint32_t license_mask = 0;
      for (size_t li = 0; li < 0x10; ++li) {
        if (device->header().header.licenses[li].license_flags) {
          license_mask |= device->header().header.licenses[li].license_bits;
        }
      }
      device.reset();

      if (pkg_ok) {
        rex::system::xam::XCONTENT_AGGREGATE_DATA content_data{};
        content_data.device_id = static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD);
        content_data.content_type = rex::system::XContentType::kMarketplaceContent;
        content_data.title_id = kAsuraTitleId;
        content_data.xuid = 0;
        content_data.set_file_name(pkg.filename);
        content_data.set_display_name(pkg.display_name_u16);
        pkg_ok = WriteContentHeaderFileDirect(user_root, content_data, license_mask);
      }
    } else {
      // Loose file inside Update/ -> copy to user_root/update/<rel_path>
      const fs::path dest_path = user_root / "update" / pkg.rel_path;
      fs::create_directories(dest_path.parent_path(), ec);
      ec.clear();

      FILE* in_f = rex::filesystem::OpenFile(pkg.src_path, "rb");
      FILE* out_f = in_f ? rex::filesystem::OpenFile(dest_path, "wb") : nullptr;
      if (!in_f || !out_f) {
        if (in_f) std::fclose(in_f);
        if (out_f) std::fclose(out_f);
        pkg_ok = false;
      } else {
        while (true) {
          if (stop_requested_.load(std::memory_order_acquire)) {
            std::fclose(in_f);
            std::fclose(out_f);
            return;
          }
          const size_t r = std::fread(copy_buf.data(), 1, kChunkSize, in_f);
          if (r == 0) {
            break;
          }
          if (std::fwrite(copy_buf.data(), 1, r, out_f) != r) {
            pkg_ok = false;
            break;
          }
          const uint64_t done =
              bytes_done_.fetch_add(uint64_t(r), std::memory_order_relaxed) + uint64_t(r);
          const double done_mb = double(done) / (1024.0 * 1024.0);
          const double tot_mb = double(bytes_total_.load(std::memory_order_relaxed)) / (1024.0 * 1024.0);
          std::lock_guard lock(status_mtx_);
          status_detail_ = fmt::format("[{}/{}] {} ({:.1f} / {:.1f} MB)", i + 1, queue_.size(),
                                       pkg.filename, done_mb, tot_mb);
        }
        if (std::ferror(in_f)) {
          pkg_ok = false;
        }
        std::fclose(in_f);
        if (std::fclose(out_f) != 0) {
          pkg_ok = false;
        }
      }
    }

    if (pkg_ok) {
      {
        std::lock_guard lock(status_mtx_);
        status_detail_ = fmt::format("[{}/{}] Deleting raw file: {}", i + 1, queue_.size(), pkg.filename);
      }
      fs::remove(pkg.src_path, ec);
      if (ec) {
        REXLOG_WARN("[Installer] Installed {} but failed to remove raw file {}: {}",
                    pkg.filename, rex::path_to_utf8(pkg.src_path), ec.message());
        ec.clear();
      } else {
        REXLOG_INFO("[Installer] Installed & deleted raw package: {} ({})",
                    pkg.filename, pkg.display_name);
      }
    } else {
      REXLOG_ERROR("[Installer] Failed to install {}; raw file preserved", pkg.filename);
    }
  }

  // Remove any stale installed.txt markers in scanned Update/dlc directories.
  for (const auto& dir : scanned_dirs_) {
    fs::remove(dir / "installed.txt", ec);
    ec.clear();
  }

  bytes_done_.store(bytes_total_.load(std::memory_order_relaxed), std::memory_order_release);
  {
    std::lock_guard lock(status_mtx_);
    status_title_ = "Update & DLC Installation Complete!";
    status_detail_ = "All packages installed and raw files cleaned up.";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  install_finished_.store(true, std::memory_order_release);
}

void AsuraStartupDialog::EnterIntroStage() {
  anim_base_ = g_intro_anim_start;
  anim_size_ = size_t(g_intro_anim_end - g_intro_anim_start);
  anim_frames_.clear();
  anim_cum_ms_.clear();

  if (anim_size_ < sizeof(AwifHeader)) {
    REXLOG_ERROR("[Intro] Embedded intro animation missing");
    FinishAndResume();
    return;
  }

  AwifHeader hdr{};
  std::memcpy(&hdr, anim_base_, sizeof(hdr));
  if (std::memcmp(hdr.magic, "AWIF", 4) != 0 || hdr.version != 1 || hdr.frame_count == 0 ||
      sizeof(AwifHeader) + size_t(hdr.frame_count) * sizeof(AwifFrameEntry) > anim_size_) {
    REXLOG_ERROR("[Intro] Invalid AWIF header");
    FinishAndResume();
    return;
  }

  anim_width_ = hdr.width;
  anim_height_ = hdr.height;
  anim_frames_.resize(hdr.frame_count);
  anim_cum_ms_.resize(hdr.frame_count);
  uint32_t acc = 0;
  for (uint32_t i = 0; i < hdr.frame_count; ++i) {
    AwifFrameEntry e{};
    std::memcpy(&e, anim_base_ + sizeof(AwifHeader) + i * sizeof(AwifFrameEntry), sizeof(e));
    anim_frames_[i] = {e.offset, e.size, std::max<uint32_t>(10, e.duration_ms)};
    acc += anim_frames_[i].duration_ms;
    anim_cum_ms_[i] = acc;
  }
  anim_total_ms_ = std::max<uint32_t>(1, acc);

  wanted_frame_idx_.store(0, std::memory_order_release);
  ready_frame_idx_ = -1;
  uploaded_frame_idx_ = -1;
  intro_stop_.store(false, std::memory_order_release);
  intro_timer_started_ = false;
  b_press_count_ = 0;
  b_released_once_ = false;
  prev_b_down_ = false;

  stage_ = Stage::kIntro;
  intro_decode_thread_ = std::thread([this] { IntroDecodeWorker(); });
}

void AsuraStartupDialog::IntroDecodeWorker() {
  int last_decoded = -1;
  while (!intro_stop_.load(std::memory_order_acquire)) {
    const int target = wanted_frame_idx_.load(std::memory_order_acquire);
    if (target == last_decoded || target < 0 || size_t(target) >= anim_frames_.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const auto& meta = anim_frames_[size_t(target)];
    if (uint64_t(meta.offset) + meta.size > anim_size_) {
      last_decoded = target;
      continue;
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgba =
        rex::ui::DecodeImageRGBA(anim_base_ + meta.offset, meta.size, w, h);
    if (!rgba.empty() && w > 0 && h > 0) {
      std::lock_guard lock(frame_mtx_);
      ready_rgba_.swap(rgba);
      ready_frame_idx_ = target;
    }
    last_decoded = target;
  }
}

void AsuraStartupDialog::FinishAndResume() {
  intro_stop_.store(true, std::memory_order_release);
  if (intro_decode_thread_.joinable()) {
    intro_decode_thread_.join();
  }
  installing_tex_.reset();
  intro_tex_.reset();
  {
    std::lock_guard lock(frame_mtx_);
    ready_rgba_.clear();
    ready_rgba_.shrink_to_fit();
  }
  stage_ = Stage::kDone;
  renut::gpu::OverlayDrawer::ClearLatest();
  rex::ui::SetGameInputBlocked(false);

  if (on_done_) {
    auto cb = std::move(on_done_);
    on_done_ = nullptr;
    cb(paths_);
  }
}

void AsuraStartupDialog::OnDraw(ImGuiIO& io) {
  if (stage_ == Stage::kInstalling) {
    if (install_finished_.load(std::memory_order_acquire)) {
      if (installer_thread_.joinable()) {
        installer_thread_.join();
      }
      installing_tex_.reset();
      EnterIntroStage();
      if (stage_ != Stage::kIntro) {
        return;
      }
    } else {
      DrawInstalling(io);
      return;
    }
  }

  if (stage_ == Stage::kIntro) {
    DrawIntro(io);
  }
}

void AsuraStartupDialog::DrawInstalling(ImGuiIO& io) {
  rex::ui::ImmediateDrawer* immediate = immediate_drawer_ ? immediate_drawer_() : nullptr;
  if (!installing_tex_ && !installing_tex_tried_ && immediate) {
    installing_tex_tried_ = true;
    const size_t jpg_len = size_t(g_installing_jpg_end - g_installing_jpg_start);
    if (jpg_len > 0) {
      int w = 0, h = 0;
      std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(g_installing_jpg_start, jpg_len, w, h);
      if (!rgba.empty() && w > 0 && h > 0) {
        installing_tex_ = immediate->CreateTexture(
            uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
      }
    }
  }

  const float W = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
  const float H = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 720.0f;
  const float scale = H / 720.0f;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##asura_installing", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, H), IM_COL32(8, 8, 12, 255));
    if (installing_tex_) {
      dl->AddImage(reinterpret_cast<ImTextureID>(installing_tex_.get()), ImVec2(0.0f, 0.0f),
                   ImVec2(W, H));
    }

    // Bottom translucent dark panel
    const float panel_x0 = W * 0.05f;
    const float panel_x1 = W * 0.95f;
    const float panel_y0 = H * 0.72f;
    const float panel_y1 = H * 0.955f;
    const float pad_x = 24.0f * scale;
    const float pad_y = 14.0f * scale;

    dl->AddRectFilled(ImVec2(panel_x0, panel_y0), ImVec2(panel_x1, panel_y1),
                      IM_COL32(10, 10, 14, 218), 12.0f * scale);
    dl->AddRect(ImVec2(panel_x0, panel_y0), ImVec2(panel_x1, panel_y1),
                IM_COL32(235, 118, 28, 225), 12.0f * scale, 0, 2.0f * scale);

    std::string title, detail;
    {
      std::lock_guard lock(status_mtx_);
      title = status_title_;
      detail = status_detail_;
    }
    const uint64_t done = bytes_done_.load(std::memory_order_relaxed);
    const uint64_t total = std::max<uint64_t>(bytes_total_.load(std::memory_order_relaxed), 1);
    const float frac = std::clamp(float(double(done) / double(total)), 0.0f, 1.0f);
    const float pct = frac * 100.0f;

    ImFont* font = ImGui::GetFont();
    const float title_sz = 24.0f * scale;
    const float pct_sz = 26.0f * scale;
    const float detail_sz = 18.0f * scale;
    const float hint_sz = 16.0f * scale;

    char pct_buf[24];
    std::snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct);

    const float text_x = panel_x0 + pad_x;
    const float right_x = panel_x1 - pad_x;
    const float top_y = panel_y0 + pad_y;

    dl->AddText(font, title_sz, ImVec2(text_x, top_y), IM_COL32(255, 240, 220, 255), title.c_str());
    const ImVec2 pct_size = font->CalcTextSizeA(pct_sz, FLT_MAX, 0.0f, pct_buf);
    dl->AddText(font, pct_sz, ImVec2(right_x - pct_size.x, top_y - 1.0f * scale),
                IM_COL32(255, 168, 46, 255), pct_buf);

    // Real-time progress bar
    const float bar_y0 = top_y + title_sz + 10.0f * scale;
    const float bar_h = 22.0f * scale;
    const float bar_y1 = bar_y0 + bar_h;
    dl->AddRectFilled(ImVec2(text_x, bar_y0), ImVec2(right_x, bar_y1),
                      IM_COL32(36, 34, 42, 245), 6.0f * scale);
    if (frac > 0.001f) {
      const float fill_x1 = text_x + (right_x - text_x) * frac;
      dl->AddRectFilledMultiColor(ImVec2(text_x, bar_y0), ImVec2(fill_x1, bar_y1),
                                  IM_COL32(220, 78, 16, 255), IM_COL32(255, 184, 52, 255),
                                  IM_COL32(255, 184, 52, 255), IM_COL32(220, 78, 16, 255));
    }
    dl->AddRect(ImVec2(text_x, bar_y0), ImVec2(right_x, bar_y1),
                IM_COL32(255, 150, 50, 170), 6.0f * scale, 0, 1.5f * scale);

    // Current detail line
    const float detail_y = bar_y1 + 10.0f * scale;
    dl->AddText(font, detail_sz, ImVec2(text_x, detail_y), IM_COL32(220, 224, 232, 255),
                detail.c_str(), nullptr, right_x - text_x);

    // Settings / Debug menu hint at bottom of panel
    const char* hint = "Tip: Press + and - together anytime to open Settings / Debug Menu";
    const float hint_y = panel_y1 - pad_y - hint_sz + 2.0f * scale;
    dl->AddText(font, hint_sz, ImVec2(text_x, hint_y), IM_COL32(245, 195, 130, 220), hint);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void AsuraStartupDialog::DrawIntro(ImGuiIO& io) {
  // Check B-button mashing (Nintendo Switch physical B = ImGuiKey_GamepadFaceDown).
  bool b_down = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
#if !REX_PLATFORM_SWITCH
  b_down = b_down || ImGui::IsKeyDown(ImGuiKey_B) || ImGui::IsKeyDown(ImGuiKey_Enter);
#endif
  if (!b_down) {
    b_released_once_ = true;
  }
  const bool b_rising = b_down && !prev_b_down_;
  prev_b_down_ = b_down;

  if (b_released_once_ && b_rising) {
    ++b_press_count_;
    if (b_press_count_ >= kRequiredBPresses) {
      FinishAndResume();
      return;
    }
  }

  const auto now = std::chrono::steady_clock::now();
  if (!intro_timer_started_) {
    intro_start_time_ = now;
    intro_timer_started_ = true;
  }
  const uint64_t elapsed_ms =
      uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - intro_start_time_).count());
  const uint32_t loop_ms = uint32_t(elapsed_ms % anim_total_ms_);

  auto it = std::upper_bound(anim_cum_ms_.begin(), anim_cum_ms_.end(), loop_ms);
  int target_idx = int(std::distance(anim_cum_ms_.begin(), it));
  if (target_idx >= int(anim_frames_.size())) {
    target_idx = int(anim_frames_.size()) - 1;
  }
  wanted_frame_idx_.store(target_idx, std::memory_order_release);

  // Pull newly decoded RGBA frame from worker without stalling UI thread.
  rex::ui::ImmediateDrawer* immediate = immediate_drawer_ ? immediate_drawer_() : nullptr;
  if (immediate) {
    std::vector<uint8_t> upload_rgba;
    int upload_idx = -1;
    {
      std::lock_guard lock(frame_mtx_);
      if (ready_frame_idx_ >= 0 && ready_frame_idx_ != uploaded_frame_idx_ && !ready_rgba_.empty()) {
        upload_rgba.swap(ready_rgba_);
        upload_idx = ready_frame_idx_;
      }
    }
    if (!upload_rgba.empty() && upload_idx >= 0) {
      intro_tex_ = immediate->CreateTexture(anim_width_, anim_height_,
                                            rex::ui::ImmediateTextureFilter::kLinear, false,
                                            upload_rgba.data());
      uploaded_frame_idx_ = upload_idx;
    }
  }

  const float W = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
  const float H = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 720.0f;
  const float scale = H / 720.0f;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##asura_intro", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, H), IM_COL32(0, 0, 0, 255));
    if (intro_tex_) {
      const float src_aspect = float(anim_width_) / float(anim_height_);
      const float dst_aspect = W / H;
      ImVec2 p0(0.0f, 0.0f), p1(W, H);
      if (dst_aspect > src_aspect) {
        const float draw_w = H * src_aspect;
        p0.x = (W - draw_w) * 0.5f;
        p1.x = p0.x + draw_w;
      } else if (dst_aspect < src_aspect) {
        const float draw_h = W / src_aspect;
        p0.y = (H - draw_h) * 0.5f;
        p1.y = p0.y + draw_h;
      }
      dl->AddImage(reinterpret_cast<ImTextureID>(intro_tex_.get()), p0, p1);
    }

    // Subtle 6px bottom bar showing B-mash progress (20 presses) without blocking baked GIF text.
    const float bar_h = 6.0f * scale;
    const float frac = std::clamp(float(b_press_count_) / float(kRequiredBPresses), 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(0.0f, H - bar_h), ImVec2(W, H), IM_COL32(20, 20, 24, 180));
    if (frac > 0.0f) {
      dl->AddRectFilledMultiColor(ImVec2(0.0f, H - bar_h), ImVec2(W * frac, H),
                                  IM_COL32(225, 80, 15, 255), IM_COL32(255, 200, 60, 255),
                                  IM_COL32(255, 200, 60, 255), IM_COL32(225, 80, 15, 255));
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace asura
