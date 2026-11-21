// Fight Night Champion: the ReXGlue application object.
//
// FNC Reborn's FncApp (project/src/fnc_app.h, commit 7f68ee3, Apache-2.0, NOTICE.fnc-reborn: window title, F1 FPS
// overlay, crash reporter / sampler) merged with the sa2-nx bench pieces this port reuses (game path search, video
// capture, guest image dump). Stage 1 has no native renderer: the emulated Xenos GPU draws.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "profiler/sa2_cpu_profile_pc.h"
#include "sa2_capture.h"
#include "sa2_pgo.h"

#if defined(_WIN32)
#include <windows.h>
#include "fnc_modules.h"
#endif

// Stage 2: Renut's Direct3D-level renderer (src/renut_engine/gpu, retargeted to FNC: RENUT_GAME_FNC), chosen at boot
// with sa2_renderer = "renut". The wiring is sa2-nx's Sa2App's (src/sa2_app.h).
#if SA2_RENUT_RENDERER
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/banjo_renut_capture.h"
#endif

namespace fnc_prof {
void Start(const void* func_mappings);  // fnc/fnc_sampler.cpp
}
namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui
namespace fnc_fps {
void Install(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);  // fnc/fnc_fps_overlay.cpp
}

class FncApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // "fnc-nx": fnc-nx.toml next to the executable and its own save folder, apart from FNC Reborn's (which moves a
  // Documents\fnc folder to Documents\FNCReborn at start, so the plain "fnc" name would be taken from us).
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FncApp>(new FncApp(ctx, "fnc-nx", PPCImageConfig));
  }

  void OnPostInitLogging() override {
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
  }

  // Without --game_data_root: a game_root/ folder next to the executable or above it (the repository's
  // assets/game_root on a PC build tree). An explicit path always wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      for (const auto& sub : {std::filesystem::path("game_root"), std::filesystem::path("assets") / "game_root"}) {
        auto pack = candidate / sub;
        pack += ".nxpack";
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec) ||
            std::filesystem::is_regular_file(pack, ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (window()) window()->SetTitle("fnc-nx (dev bench)");
    fnc_fps::Install(drawer, window());
  }

#if SA2_RENUT_RENDERER
  // With Renut's renderer chosen: no SDK graphics system and no GPU plugin (ReXApp loads the plugin only when
  // config.graphics is empty and this returns before it is chosen); Renut has its own window swapchain and thread.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("graphics: Renut's Direct3D-level renderer (sa2_renderer = renut: no PM4 ring, no EDRAM)");
    }
  }
  // Renut draws the SDK's menus through its own drawer, resizes its swapchain itself, and starts once the window
  // exists (here, before the game: the paths are final).
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return rex::ReXApp::OnCreateImmediateDrawer();
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active()) renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
#endif

  void OnPostSetup() override {
    fnc_prof::Start(PPCImageConfig.func_mappings);
    nfsmw::perfil_pc::Arrancar();
#if SA2_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      // Renut's Swap hook counts the game frames itself (g_sa2_game_frames): no swap-count poller.
      sa2::capture::StartWithSource([](rex::ui::RawImage& image) { return renut::gpu::CaptureFrame(image); });
      sa2::pgo::Start();
      return;
    }
#endif
    StartFrameCounter();
    sa2::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
    sa2::pgo::Start();
  }

  void OnWindowFocusChanged(bool focused) override {
    if (!focused) sa2::pgo::DumpNow();
  }

  void OnShutdown() override {
#if SA2_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
    frame_counter_stop_ = true;
    if (frame_counter_.joinable()) frame_counter_.join();
    nfsmw::perfil_pc::Parar();
    sa2::capture::Stop();
    sa2::pgo::Stop();
  }

  // Analysis aid: with SA2_DUMP_IMAGE=<file>, write the loaded guest image (tools/pointer_scan.py and friends).
  void OnPostLoadXexImage() override {
    if (!runtime() || !runtime()->memory()) {
      return;
    }
    const char* path = std::getenv("SA2_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const uint8_t* image = runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
      REXLOG_INFO("guest image written to {}", path);
    }
  }

 private:
  // Stage 1 stand-in for the flip hook that drives g_sa2_game_frames (sa2-nx hooked SA2's flip; FNC's is found in
  // stage 2): the emulated GPU's XE_SWAP count, exported by the GPU plugin (sdk/src/graphics/fnc_swap_counter.cpp).
  void StartFrameCounter() {
#if defined(_WIN32)
    using SwapCountFn = uint64_t (*)();
    HMODULE m = FncSdkModule(L"rexgpu-xenos") /* fnc-nx: build-type suffix */;
    auto fn = m ? reinterpret_cast<SwapCountFn>(GetProcAddress(m, "fnc_gpu_swap_count")) : nullptr;
    if (!fn) {
      REXLOG_WARN("fnc: fnc_gpu_swap_count not found, video capture has no frame counter");
      return;
    }
    frame_counter_ = std::thread([this, fn] {
      while (!frame_counter_stop_) {
        g_sa2_game_frames.store(fn(), std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
#endif
  }

  std::thread frame_counter_;
  std::atomic<bool> frame_counter_stop_{false};
};
