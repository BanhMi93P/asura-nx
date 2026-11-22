#pragma once
#include "profiler/banjo_pc_profiler.h"
#include "banjo_pgo.h"
#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include "renut_engine/overlays/fps_overlay_dialog.h"
#include "renut_engine/renut_logging.h"
#include "renut_engine/overlays/renut_logging_overlay.h"
#if !REX_PLATFORM_SWITCH
#include "renut_engine/overlays/path_setup_wizard.h"
#include "renut_engine/overlays/mnk_controls_dialog.h"
#endif
#include "renut_engine/mnk_controls.h"
#include "renut_engine/Timer.h"
#include "renut_engine/Fps.h"
#include "renut_engine/hooks.h"
#include "renut_engine/dlc_install.h"
#include "asura_startup_overlay.h"
#include "profiler/profiler.h"
#include "profiler/profiler_overlay.h"
#include "game_menu.h"
#include "me/me_native_system.h"
#if BANJO_RENUT_RENDERER
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/banjo_renut_capture.h"
#endif
#include "cpu/banjo_enhancements.h"
#if defined(KATAMARI_BUILTIN_CONFIG)
#include "katamari_builtin_config.h"
#endif
namespace katamari::stage_diag {
void Start();
}
namespace blur::small_alloc {
void Enable();
}
#include "native/nfsmw_nativo_captura.h"
#if !REX_PLATFORM_SWITCH
#include "profiler/nfsmw_perfil_pc.h"
#endif
#include <rex/ui/window.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

// Asura's Wrath integration
#include "game/frame_rate.h"
#include "game/skip_intro.h"
#include "input/pad_remap.h"

REXCVAR_DECLARE(bool, banjo_show_fps);

REXCVAR_DECLARE(bool, banjo_mesa_disk_cache_off);
REXCVAR_DECLARE(bool, masseffect_nvk_cache_per_stage);
REXCVAR_DECLARE(bool, banjo_gpu_boost);
#if BANJO_RENUT_RENDERER
REXCVAR_DECLARE(bool, banjo_renut_linked_stages);
#endif
REXCVAR_DECLARE(bool, banjo_gpu_zcull_greater);
#if REX_PLATFORM_SWITCH
extern "C" {
void RexSwitchApmRequestGpuMhz(int mhz);
void RexSwitchApmApply(void);
}
#endif

extern "C" const char* RexAppBuildStamp(void);

class RenutApp : public rex::ReXApp {
public:
    RenutApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
        : rex::ReXApp(ctx, name, info), app_name_(name) {}

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "asurawrath_nx", PPCImageConfig));
    }

    std::string_view BuiltInConfig() const override {
#if defined(KATAMARI_BUILTIN_CONFIG)
        return kKatamariBuiltInConfig;
#else
        return {};
#endif
    }
    std::string_view DefaultConfigFile() const override {
#if defined(KATAMARI_BUILTIN_CONFIG)
        return kKatamariPlayerConfig;
#else
        return {};
#endif
    }

    void OnConfigureFonts(ImFontAtlas* atlas) override { blur::AddGameMenuFonts(atlas); }

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
#if BANJO_RENUT_RENDERER
        if (!banjo::renut_gpu::Active()) return;
#endif
        fps_dialog_ = std::make_unique<FpsOverlayDialog>(drawer);
        fps_dialog_->fpsManager = &fpsManager;
        fpsManager.showFPS = REXCVAR_GET(banjo_show_fps);
        fpsManager.GetCreateCounter("Shown");
        drawer->AddDialog(fps_dialog_.get());
        drawer->AddDialog(new RenuLogOverlayDialog(drawer));
        profiler_dialog_ = std::make_unique<ProfilerOverlayDialog>(drawer);
        drawer->AddDialog(profiler_dialog_.get());
        game_menu_ = std::make_unique<blur::GameMenuDialog>(drawer, config_path(), [this] { return runtime(); },
                                                            [this] { return immediate_drawer(); });
        startup_dialog_ = std::make_unique<asura::AsuraStartupDialog>(
            drawer, [this] { return immediate_drawer(); });
#if !REX_PLATFORM_SWITCH
        path_wizard_ = new PathSetupWizard(drawer);
        drawer->AddDialog(path_wizard_);

        mnk_dialog_ = std::make_unique<MnkControlsDialog>(drawer);
        drawer->AddDialog(mnk_dialog_.get());
        renut::mnk::AttachWindow(window());
#endif
    }

    void OnPreSetup(rex::RuntimeConfig& config) override {
        // Asura's Wrath controller input system with pad remapping
        config.input_factory = [](bool tool_mode) { return asura::input::CreateInputSystem(tool_mode); };

        banjo::ApplyMasterSwitches();
#if REX_PLATFORM_SWITCH
        if (banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_mesa_disk_cache_off))) {
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        }
        if (banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_zcull_greater))) {
            setenv("NVK_ZCULL", "greater", 1);
        }
        bool per_stage_cache = banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu,
                                                        REXCVAR_GET(masseffect_nvk_cache_per_stage));
#if BANJO_RENUT_RENDERER
        if (per_stage_cache && banjo::renut_gpu::Active() &&
            banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_linked_stages))) {
            per_stage_cache = false;
        }
#endif
        if (per_stage_cache) {
            setenv("NVK_SWITCH_STAGE_CACHE", "1", 1);
        }
        if (banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_boost))) {
            RexSwitchApmRequestGpuMhz(460);
            RexSwitchApmApply();
        }
#endif

#if BANJO_RENUT_RENDERER
        if (banjo::renut_gpu::Active()) {
            renut::gpu::ConfigureRuntime(config);
            banjo::profiler::SetRendererInfo("Renut native (Direct3D level, plume)", "");
            return;
        }
#endif
        if (me::native::Enabled()) {
            config.graphics = me::native::CreateGraphicsSystem();
            banjo::profiler::SetRendererInfo("Native Vulkan (PM4 ring)", "");
        }
    }

#if BANJO_RENUT_RENDERER
    std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
        if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
        return rex::ReXApp::OnCreateImmediateDrawer();
    }
    void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
        rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
        if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
    }
    void StartRenutRenderer() {
        if (banjo::renut_gpu::Active())
            renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    }
#else
    void StartRenutRenderer() {}
#endif

    void OnPostSetup() override {
        REXLOG_INFO("[build] {}", RexAppBuildStamp());
        blur::small_alloc::Enable();
        if (!SaveDataExists()) {
            REXLOG_INFO("No Asura's Wrath save found; relying on switch_message_box_focused for first setup");
        }
        banjo::LogEnhancements();
        katamari::stage_diag::Start();
        banjo::pc_profiler::Start();
        banjo::profiler::Start();
#if BANJO_RENUT_RENDERER
        if (banjo::renut_gpu::Active()) {
            nfsmw::captura::Arrancar(nfsmw::captura::Capturador(
                [](rex::ui::RawImage& image) { return renut::gpu::CaptureFrame(image); }));
        } else
#endif
        nfsmw::captura::Arrancar([this]() -> rex::ui::Presenter* {
            const auto* rt = runtime();
            const auto* graphics = rt ? rt->graphics_system() : nullptr;
            return graphics ? graphics->presenter() : nullptr;
        });
#if !REX_PLATFORM_SWITCH
        nfsmw::perfil_pc::Arrancar();
#endif
        banjo::pgo::Start();
    }

    void OnPostLaunchModule(rex::system::XThread* thread) override {
        rex::ReXApp::OnPostLaunchModule(thread);
    }

    void InstallDlcAndUpdates() {
        if (!runtime() || !runtime()->kernel_state() || !runtime()->kernel_state()->content_manager()) {
            return;
        }
        std::error_code ec;
        const auto exe_dir = rex::filesystem::GetExecutableFolder();
        const auto files_dir = user_data_root_path_.empty() ? exe_dir : user_data_root_path_;

        for (const auto& dir : {exe_dir / "Update", exe_dir / "update",
                                files_dir / "Update", files_dir / "update",
                                std::filesystem::path("sdmc:/switch/asura-nx/Update"),
                                std::filesystem::path("/switch/asura-nx/Update"),
                                std::filesystem::path("/switch/asura_wrath_recomp/Update")}) {
            if (std::filesystem::is_directory(dir, ec)) {
                if (std::filesystem::exists(dir / "installed.txt", ec)) {
                    REXLOG_INFO("[Update] Directory {} already marked as installed; skipping", dir.string());
                    break;
                }
                REXLOG_INFO("[Update] Scanning directory: {}", dir.string());
                bool any_installed = false;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (entry.is_regular_file(ec) && entry.path().filename() != "installed.txt") {
                        const auto res = runtime()->kernel_state()->content_manager()->InstallContent(entry.path());
                        REXLOG_INFO("[Update] Package '{}': install result {:08X}", entry.path().filename().string(), unsigned(res));
                        any_installed = true;
                    }
                }
                if (any_installed) {
                    std::ofstream f(dir / "installed.txt");
                    f << "installed\n";
                }
                break;
            }
        }
    }

    bool SaveDataExists() const {
        std::error_code ec;
        if (user_data_root_path_.empty() ||
            !std::filesystem::is_directory(user_data_root_path_, ec)) {
            return false;
        }
        for (const auto& profile : std::filesystem::directory_iterator(user_data_root_path_, ec)) {
            if (std::filesystem::exists(
                    profile.path() / "43430817" / "00000001" / "SAVEDATA.DAT", ec)) {
                return true;
            }
        }
        return false;
    }

    void OnWindowFocusChanged(bool focused) override {
        if (!focused) banjo::pgo::DumpNow();
    }

    void OnShutdown() override {
        if (startup_dialog_) {
            startup_dialog_->Stop();
            startup_dialog_.reset();
        }
#if BANJO_RENUT_RENDERER
        if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
        banjo::pc_profiler::Stop();
        nfsmw::captura::Parar();
#if !REX_PLATFORM_SWITCH
        nfsmw::perfil_pc::Parar();
#endif
        renut::mnk::DetachWindow();
        banjo::pgo::Stop();
    }

    void OnConfigurePaths(rex::PathConfig& paths) override {
        user_data_root_path_ = paths.user_data_root;
        std::error_code ec;
        const auto exe_dir = rex::filesystem::GetExecutableFolder();

        if (!std::filesystem::exists(paths.config_path, ec)) {
            for (const auto& candidate : {
                exe_dir / "asurawrath_nx.toml",
                exe_dir / "asura_wrath_recomp.toml",
                exe_dir / "renut.toml",
                paths.user_data_root / "asurawrath_nx.toml",
                paths.user_data_root / "asura_wrath_recomp.toml",
                paths.user_data_root / "renut.toml",
                std::filesystem::path("sdmc:/switch/asura-nx/asurawrath_nx.toml"),
                std::filesystem::path("sdmc:/switch/asura-nx/asura_wrath_recomp.toml"),
                std::filesystem::path("/switch/asura-nx/asurawrath_nx.toml"),
                std::filesystem::path("/switch/asura-nx/asura_wrath_recomp.toml"),
                std::filesystem::path("/switch/asura_wrath_recomp/asurawrath_nx.toml"),
                std::filesystem::path("/switch/asura_wrath_recomp/asura_wrath_recomp.toml")}) {
                if (std::filesystem::exists(candidate, ec)) {
                    paths.config_path = candidate;
                    break;
                }
            }
            if (!std::filesystem::exists(paths.config_path, ec)) {
                paths.config_path = paths.user_data_root / paths.config_path.filename();
            }
        }

        if (paths.game_data_root.empty() || !std::filesystem::exists(paths.game_data_root, ec)) {
            const auto cur_dir = std::filesystem::current_path(ec);
            for (const auto& dir : {
#if REX_PLATFORM_SWITCH || defined(__SWITCH__)
                exe_dir / "game",
                exe_dir / "game_root",
                std::filesystem::path("/switch/asura-nx/game"),
                std::filesystem::path("/switch/asura-nx/game_root"),
                paths.user_data_root / "game",
                paths.user_data_root / "game_root",
                std::filesystem::path("/switch/asura_wrath_recomp/game"),
                std::filesystem::path("/switch/asura_wrath_recomp/game_root"),
#endif
                cur_dir / "game",
                cur_dir / ".." / "game",
                cur_dir / ".." / ".." / "game",
                exe_dir / "game",
                exe_dir / ".." / "game",
                exe_dir / ".." / ".." / "game",
                cur_dir / "extracted",
                exe_dir / "extracted",
                std::filesystem::path("G:/!Project/!port/Asuras Wrath/game"),
                cur_dir / "game_root",
                exe_dir / "game_root"}) {
                if (std::filesystem::is_regular_file(dir / "default.xex", ec) ||
                    std::filesystem::is_directory(dir, ec)) {
                    paths.game_data_root = dir;
                    break;
                }
            }
        }

        if (paths.update_data_root.empty() || !std::filesystem::exists(paths.update_data_root, ec)) {
            const auto cur_dir = std::filesystem::current_path(ec);
            for (const auto& dir : {
                cur_dir / "Update", cur_dir / "update",
                cur_dir / "dlc-update" / "update",
                cur_dir / ".." / "dlc-update" / "update",
                exe_dir / "Update", exe_dir / "update",
                exe_dir / ".." / "dlc-update" / "update",
                exe_dir / ".." / ".." / "dlc-update" / "update",
                std::filesystem::path("G:/!Project/!port/Asuras Wrath/dlc-update/update"),
                paths.game_data_root / "Update", paths.game_data_root / "update"}) {
                if (!dir.empty() && std::filesystem::is_regular_file(dir / "default.xexp", ec)) {
                    paths.update_data_root = dir;
                    break;
                }
            }
        }

        renut::dlc::SetUserDataRoot(paths.user_data_root);
    }

#if !REX_PLATFORM_SWITCH
    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        StartRenutRenderer();
        std::error_code given_ec;
        const bool given = !defaults.game_data_root.empty() &&
                           (std::filesystem::is_regular_file(defaults.game_data_root / "default.xex", given_ec) ||
                            std::filesystem::is_directory(defaults.game_data_root, given_ec));
        if (given || !path_wizard_) {
            renut::dlc::SetUserDataRoot(defaults.user_data_root);
            return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
        }
        path_wizard_->Init(app_name_, defaults, [resume](rex::PathConfig resolved) {
            renut::dlc::SetUserDataRoot(resolved.user_data_root);
            resume(resolved);
            });
        return std::nullopt;
    }
#else
    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        StartRenutRenderer();
        if (startup_dialog_) {
            startup_dialog_->Begin(defaults, [this, resume](rex::PathConfig resolved) {
                renut::dlc::SetUserDataRoot(resolved.user_data_root);
                app_context().CallInUIThreadDeferred([resume, resolved = std::move(resolved)]() mutable {
                    resume(std::move(resolved));
                });
            });
            return std::nullopt;
        }
        return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
    }
#endif

private:
    std::string      app_name_;
    std::filesystem::path user_data_root_path_;
#if !REX_PLATFORM_SWITCH
    PathSetupWizard* path_wizard_ = nullptr;
    std::unique_ptr<MnkControlsDialog> mnk_dialog_;
#endif
    std::unique_ptr<FpsOverlayDialog> fps_dialog_;
    std::unique_ptr<ProfilerOverlayDialog> profiler_dialog_;
    std::unique_ptr<blur::GameMenuDialog> game_menu_;
    std::unique_ptr<asura::AsuraStartupDialog> startup_dialog_;
};
