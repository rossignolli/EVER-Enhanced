#include "CrashHandler.h"
#include "EncoderSession.h"
#include "logger.h"
#include "polyhook2/ErrorLog.hpp"
#include "script.h"
#include "stdafx.h"
#include "util.h"

#include <filesystem>

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

namespace ever {
bool isExportActive();
}

static bool g_overlay_was_open_before_export = false;
static reshade::api::effect_runtime* g_current_runtime = nullptr;
static bool g_script_registered = false;
static bool g_initialized = false;
static bool g_reshade_registered = false;
static bool g_crash_handler_installed = false;

static bool on_reshade_open_overlay(reshade::api::effect_runtime* runtime, bool open,
                                    reshade::api::input_source source) {
    g_current_runtime = runtime;

    if (ever::isExportActive() && open) {
        return true;
    }

    return false;
}

static void on_reshade_present(reshade::api::effect_runtime* runtime) {
    g_current_runtime = runtime;
    ever::OnReshadePresent(runtime);
}

static void on_reshade_init_runtime(reshade::api::effect_runtime* runtime) {
    g_current_runtime = runtime;
    ever::OnReshadeInitRuntime(runtime);
}

static void on_reshade_destroy_runtime(reshade::api::effect_runtime* runtime) {
    ever::OnReshadeDestroyRuntime(runtime);
    if (g_current_runtime == runtime) g_current_runtime = nullptr;
}

static void on_reshade_begin_effects(reshade::api::effect_runtime* runtime,
                                     reshade::api::command_list* commandList,
                                     reshade::api::resource_view rtv,
                                     reshade::api::resource_view) {
    ever::OnReshadeBeginEffects(runtime, commandList, rtv);
}

static void on_reshade_bind_render_targets(reshade::api::command_list* cmdList, uint32_t count,
                                           const reshade::api::resource_view* rtvs,
                                           reshade::api::resource_view dsv) {
    if (cmdList == nullptr) return;

    if (rtvs != nullptr) {
        for (uint32_t index = 0; index < count; ++index) {
            ever::OnReshadeRenderTarget(cmdList->get_device(), rtvs[index]);
        }
    }
    ever::OnReshadeDepthStencil(cmdList->get_device(), dsv);
}

static void on_reshade_begin_render_pass(reshade::api::command_list* cmdList, uint32_t count,
                                         const reshade::api::render_pass_render_target_desc* renderTargets,
                                         const reshade::api::render_pass_depth_stencil_desc* depthStencil) {
    if (cmdList == nullptr) return;

    if (renderTargets != nullptr) {
        for (uint32_t index = 0; index < count; ++index) {
            ever::OnReshadeRenderTarget(cmdList->get_device(), renderTargets[index].view);
        }
    }
    if (depthStencil != nullptr)
        ever::OnReshadeDepthStencil(cmdList->get_device(), depthStencil->view);
}

static void on_reshade_barrier(reshade::api::command_list* cmdList, uint32_t count,
                               const reshade::api::resource* resources,
                               const reshade::api::resource_usage*,
                               const reshade::api::resource_usage* newStates) {
    if (cmdList == nullptr) return;

    ever::OnReshadeBarrier(cmdList->get_device(), count, resources, newStates);
}

static void on_reshade_destroy_resource(reshade::api::device*, reshade::api::resource resource) {
    ever::OnReshadeDestroyResource(resource);
}

static bool on_reshade_copy_resource(reshade::api::command_list* cmdList,
                                     reshade::api::resource source,
                                     reshade::api::resource destination) {
    if (cmdList != nullptr)
        ever::OnReshadeCopyResource(cmdList->get_device(), source, destination, "copy_resource");
    return false;
}

static bool on_reshade_copy_texture_region(reshade::api::command_list* cmdList,
                                           reshade::api::resource source, uint32_t,
                                           const reshade::api::subresource_box*,
                                           reshade::api::resource destination, uint32_t,
                                           const reshade::api::subresource_box*,
                                           reshade::api::filter_mode) {
    if (cmdList != nullptr)
        ever::OnReshadeCopyResource(cmdList->get_device(), source, destination, "copy_texture_region");
    return false;
}

static bool on_reshade_resolve_texture_region(reshade::api::command_list* cmdList,
                                              reshade::api::resource source, uint32_t,
                                              const reshade::api::subresource_box*,
                                              reshade::api::resource destination, uint32_t,
                                              int32_t, int32_t, int32_t, reshade::api::format) {
    if (cmdList != nullptr)
        ever::OnReshadeCopyResource(cmdList->get_device(), source, destination, "resolve_texture_region");
    return false;
}

class PolyHookLogger : public PLH::Logger {
    void log(const std::string& msg, PLH::ErrorLevel level) override {
        switch (level) {
        case PLH::ErrorLevel::INFO:
            LOG(LL_TRC, msg);
            break;
        case PLH::ErrorLevel::WARN:
            LOG(LL_WRN, msg);
            break;
        case PLH::ErrorLevel::SEV:
            LOG(LL_ERR, msg);
            break;
        case PLH::ErrorLevel::NONE:
        default:
            LOG(LL_DBG, msg);
            break;
        }
    }
};

static void draw_eve_settings(reshade::api::effect_runtime* runtime) {
    const bool is_rendering = ever::isExportActive();

    ImGui::TextUnformatted("EVER - Extended Video Export Revived");
    ImGui::Text("Game edition: %s", ever::isEnhancedEdition() ? "GTA V Enhanced" : "GTA V Legacy");
    if (ever::isEnhancedEdition()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.0f, 1.0f));
        ImGui::TextWrapped("Enhanced compatibility mode: DX12 video/audio export, clean pre-UI capture, "
                           "OpenEXR color/depth, custom FPS and multi-sample motion blur are enabled. "
                           "Dual-pass audio is %s.",
                           ever::isDualPassAvailable() ? "available for testing" : "unavailable for this game build");
        ImGui::PopStyleColor();

        const float configuredFps = static_cast<float>(Config::Manager::fps.first) /
                                    static_cast<float>(std::max(1u, Config::Manager::fps.second));
        const float effectiveFps = configuredFps *
                                   (static_cast<float>(Config::Manager::motion_blur_samples) + 1.0f);
        if (effectiveFps > 60.0f) {
            if (ever::isDualPassAvailable()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 1.0f, 1.0f));
                ImGui::TextWrapped("This configuration renders at %.2f effective FPS. EVER will use an "
                                   "audio-only first pass followed by the configured video pass.", effectiveFps);
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.2f, 1.0f));
                ImGui::TextWrapped("Audio warning: this configuration renders at %.2f effective FPS, but "
                                   "the Enhanced dual-pass hooks are unavailable for this game build.", effectiveFps);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::Separator();

    if (is_rendering) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
        ImGui::TextUnformatted("[RENDER IN PROGRESS - Settings Locked]");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    ImGui::BeginDisabled(is_rendering);

    if (ImGui::Checkbox("Enable Mod", &Config::Manager::is_mod_enabled)) {
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Enable or disable the EVER mod");

    ImGui::Spacing();
    ImGui::TextUnformatted("Export Settings");
    ImGui::Separator();

    ImGui::BeginDisabled(!ever::isCustomFrameRateAvailable());

    float current_fps =
        static_cast<float>(Config::Manager::fps.first) / static_cast<float>(Config::Manager::fps.second);
    int fps_display = static_cast<int>(current_fps + 0.5f); // Round to nearest integer

    if (ImGui::InputInt("FPS", &fps_display, 1, 10)) {
        if (fps_display > 0 && fps_display <= 1000) {
            Config::Manager::fps.first = static_cast<uint32_t>(fps_display * 1000);
            Config::Manager::fps.second = 1000;
            Config::Manager::save();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frames per second for video export (e.g., 30, 60, 120)");

    ImGui::EndDisabled();
    ImGui::BeginDisabled(!ever::isCustomFrameRateAvailable());

    ImGui::Spacing();
    int mb_samples = static_cast<int>(Config::Manager::motion_blur_samples);
    if (ImGui::SliderInt("Motion Blur Samples", &mb_samples, 0, 255)) {
        Config::Manager::motion_blur_samples = static_cast<uint8_t>(mb_samples);
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Number of motion blur samples (0 = disabled, higher = smoother but slower)");

    if (ImGui::SliderFloat("Motion Blur Strength", &Config::Manager::motion_blur_strength, 0.0f, 1.0f, "%.2f")) {
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Motion blur strength (0.0 = none, 1.0 = maximum)");

    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Checkbox("Export OpenEXR", &Config::Manager::export_openexr)) {
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Export a full-resolution OpenEXR sequence with RGB and depth.Z alongside video");

    if (ImGui::Checkbox("Disable Watermark", &Config::Manager::disable_watermark)) {
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(ever::isEnhancedEdition()
            ? "Enhanced captures before Rockstar's export UI/watermark; this setting is kept for preset compatibility"
            : "Remove Rockstar Editor watermark from exports");

    if (ImGui::Checkbox("Auto Reload Config", &Config::Manager::auto_reload_config)) {
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Automatically reload config when INI file changes");

    ImGui::Spacing();
    ImGui::TextUnformatted("Output Directory");
    ImGui::Separator();

    char output_path[512];
    strncpy_s(output_path, Config::Manager::output_dir.c_str(), sizeof(output_path) - 1);
    if (ImGui::InputText("##OutputDir", output_path, sizeof(output_path))) {
        Config::Manager::output_dir = output_path;
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Directory where exported videos will be saved");

    ImGui::Spacing();
    ImGui::TextUnformatted("Log Level");
    ImGui::Separator();

    const char* log_levels[] = {"Error", "Warning", "Info", "Debug", "Trace"};
    int current_log_level = static_cast<int>(Config::Manager::log_level);
    if (ImGui::Combo("Log Level", &current_log_level, log_levels, IM_ARRAYSIZE(log_levels))) {
        Config::Manager::log_level = static_cast<LogLevel>(current_log_level);
        Logger::instance().level = Config::Manager::log_level;
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set logging verbosity level");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reload Config from INI")) {
        Config::Manager::reload();
        Logger::instance().level = Config::Manager::log_level;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reload all settings from the INI file");

    ImGui::SameLine();
    if (ImGui::Button("Save All Settings")) {
        Config::Manager::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Force save all current settings to INI file");

    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Status Information");
    ImGui::Text("Current FPS: %.2f",
                static_cast<float>(Config::Manager::fps.first) / static_cast<float>(Config::Manager::fps.second));
    ImGui::Text("Export Active: %s", is_rendering ? "YES" : "NO");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        try {
            ever::crash::Initialize();
            g_crash_handler_installed = true;

            LOG(LL_NON, std::filesystem::current_path());

            const std::wstring dll_path = utf8_decode(AsiPath() + R"(\EVER\dlls\)");


            static const wchar_t* k_runtime_dlls[] = {
                L"PolyHook_2.dll",  L"OpenEXR-3_1.dll",  L"avutil-58.dll",   L"avcodec-60.dll",
                L"avformat-60.dll", L"avfilter-9.dll",   L"avdevice-60.dll", L"swresample-4.dll",
                L"swscale-7.dll",
            };

            for (auto dll_name : k_runtime_dlls) {
                const std::wstring full_path = dll_path + dll_name;
                HMODULE h = LoadLibraryExW(full_path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!h) {
                    const DWORD err = ::GetLastError();
                    LOG(LL_ERR, "Failed to load runtime DLL:", utf8_encode(full_path), " error:", err);
                    return FALSE;
                }
                LOG(LL_DBG, "Loaded runtime DLL:", utf8_encode(full_path));
            }
            LOG(LL_DBG, "Runtime DLLs loaded without changing the process DLL search path: ", utf8_encode(dll_path));

            LOG(LL_DBG, "Initializing PolyHook logger");
            PLH::Log::registerLogger(std::make_unique<PolyHookLogger>());


            if (reshade::register_addon(hModule, GetModuleHandleW(L"ReShade.asi"))) {
                LOG(LL_NON, "Successfully registered with Reshade!");
                reshade::register_overlay(nullptr, draw_eve_settings);
                reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_reshade_open_overlay);
                reshade::register_event<reshade::addon_event::init_effect_runtime>(on_reshade_init_runtime);
                reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_reshade_destroy_runtime);
                reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
                if (ever::isEnhancedEdition()) {
                    reshade::register_event<reshade::addon_event::reshade_begin_effects>(
                        on_reshade_begin_effects);
                    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
                        on_reshade_bind_render_targets);
                    reshade::register_event<reshade::addon_event::begin_render_pass>(on_reshade_begin_render_pass);
                    reshade::register_event<reshade::addon_event::barrier>(on_reshade_barrier);
                    reshade::register_event<reshade::addon_event::copy_resource>(on_reshade_copy_resource);
                    reshade::register_event<reshade::addon_event::copy_texture_region>(on_reshade_copy_texture_region);
                    reshade::register_event<reshade::addon_event::resolve_texture_region>(on_reshade_resolve_texture_region);
                    reshade::register_event<reshade::addon_event::destroy_resource>(on_reshade_destroy_resource);
                }
                g_reshade_registered = true;
            } else {
                LOG(LL_ERR, "Failed to register with Reshade! EVER requires Reshade to function.");

                return FALSE;
            }

            Config::Manager::reload();
            if (!Config::Manager::is_mod_enabled) {
                LOG(LL_NON, "Extended Video Export mod is disabled in the config file. Exiting...");
                return TRUE;
            } else {
                LOG(LL_NON, "Extended Video Export mod is enabled. Initializing...");
            }
            Logger::instance().level = Config::Manager::log_level;
            LOG_CALL(LL_DBG, ever::initialize());
            g_initialized = true;
            LOG(LL_NFO, "Registering script...");
            LOG_CALL(LL_DBG, scriptRegister(hModule, ever::ScriptMain));
            g_script_registered = true;
        } catch (const std::exception& e) {
            LOG(LL_ERR, "C++ exception during DLL initialization:", e.what());
            return FALSE;
        } catch (...) {
            LOG(LL_ERR, "Unknown exception during DLL initialization!");
            return FALSE;
        }
        break;
    case DLL_PROCESS_DETACH:
        try {
            LOG(LL_NFO, "Unregistering DXGI callback");

            if (g_script_registered) {
                LOG_CALL(LL_DBG, scriptUnregister(hModule));
            } else {
                LOG(LL_DBG, "Skipping scriptUnregister; script was never registered");
            }

            if (g_initialized) {
                LOG_CALL(LL_DBG, ever::finalize());
            }

            if (g_reshade_registered) {
                reshade::unregister_addon(hModule);
            }

            if (g_crash_handler_installed) {
                ever::crash::Cleanup();
                g_crash_handler_installed = false;
            }
        } catch (...) {
        }
        break;
    }
    return TRUE;
}
