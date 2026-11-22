#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace asura {

class AsuraStartupDialog final : public rex::ui::ImGuiDialog {
 public:
  AsuraStartupDialog(rex::ui::ImGuiDrawer* drawer,
                     std::function<rex::ui::ImmediateDrawer*()> immediate_drawer);
  ~AsuraStartupDialog() override;

  void Begin(const rex::PathConfig& defaults,
             std::function<void(rex::PathConfig)> on_done);
  void Stop();

  bool WantsFrame() const override {
    return stage_ == Stage::kInstalling || stage_ == Stage::kIntro;
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Stage {
    kIdle,
    kInstalling,
    kIntro,
    kDone,
  };

  struct QueuedPackage {
    std::filesystem::path src_path;
    std::filesystem::path rel_path;  // relative to source root (for loose Update files)
    std::string filename;
    std::string display_name;
    std::u16string display_name_u16;
    bool from_update_dir = false;
    bool is_stfs = false;
    uint32_t title_id = 0;
    uint32_t content_type = 0;
    uint64_t payload_bytes = 0;
  };

  struct IntroFrameMeta {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t duration_ms = 33;
  };

  void ScanQueuedPackages();
  void InstallWorker();
  void EnterIntroStage();
  void IntroDecodeWorker();
  void FinishAndResume();

  void DrawInstalling(ImGuiIO& io);
  void DrawIntro(ImGuiIO& io);

  std::function<rex::ui::ImmediateDrawer*()> immediate_drawer_;
  rex::PathConfig paths_{};
  std::function<void(rex::PathConfig)> on_done_;

  Stage stage_ = Stage::kIdle;

  // Stage 1: DLC & Update installer
  std::vector<QueuedPackage> queue_;
  std::vector<std::filesystem::path> scanned_dirs_;
  std::thread installer_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> install_finished_{false};
  std::atomic<uint64_t> bytes_done_{0};
  std::atomic<uint64_t> bytes_total_{1};
  std::atomic<int> pkg_index_{0};
  std::atomic<int> pkg_count_{0};
  std::mutex status_mtx_;
  std::string status_title_ = "Preparing Update & DLC installer...";
  std::string status_detail_ = "Scanning Update and dlc folders...";
  std::unique_ptr<rex::ui::ImmediateTexture> installing_tex_;
  bool installing_tex_tried_ = false;

  // Stage 2: Embedded 6.5s looping intro animation + 20x B mash gate
  const uint8_t* anim_base_ = nullptr;
  size_t anim_size_ = 0;
  uint32_t anim_width_ = 1280;
  uint32_t anim_height_ = 720;
  uint32_t anim_total_ms_ = 6500;
  std::vector<IntroFrameMeta> anim_frames_;
  std::vector<uint32_t> anim_cum_ms_;

  std::thread intro_decode_thread_;
  std::atomic<bool> intro_stop_{false};
  std::atomic<int> wanted_frame_idx_{0};
  std::mutex frame_mtx_;
  std::vector<uint8_t> ready_rgba_;
  int ready_frame_idx_ = -1;
  int uploaded_frame_idx_ = -1;
  std::unique_ptr<rex::ui::ImmediateTexture> intro_tex_;

  std::chrono::steady_clock::time_point intro_start_time_{};
  bool intro_timer_started_ = false;
  int b_press_count_ = 0;
  bool b_released_once_ = false;
  bool prev_b_down_ = false;
};

}  // namespace asura
