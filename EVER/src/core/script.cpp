// TODO: Split this codebase into multiple files.
#include "script.h"
#include "CrashHandler.h"
#include "MFUtility.h"
#include "EncoderSession.h"
#include "HookDefinitions.h"
#include "logger.h"
#include "stdafx.h"
#include "util.h"
#include "PatternScanner.h"
#include <mferror.h>
#include <ppl.h>
#include <reshade.hpp>

#include "ScanPatterns.h"
#include <DirectXTex.h>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <dxgi1_3.h>
#include <cwctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <variant>

void AutoReloadConfig();
void CreateNewExportContext();

namespace PSAccumulate {
#include <PSAccumulate.h>
}

namespace PSDivide {
#include <PSDivide.h>
}

namespace VSFullScreen {
#include <VSFullScreen.h>
}

namespace PSAccumulateEnhanced {
#include <PSAccumulateEnhanced.h>
}

namespace PSDivideEnhanced {
#include <PSDivideEnhanced.h>
}

namespace VSFullScreenEnhanced {
#include <VSFullScreenEnhanced.h>
}

using namespace Microsoft::WRL;
using namespace DirectX;

namespace {
    ComPtr<ID3D11Texture2D> pGameBackBuffer;
    ComPtr<ID3D11Texture2D> pGameBackBufferResolved;
    ComPtr<ID3D11Texture2D> pGameDepthBufferQuarterLinear;
    ComPtr<ID3D11Texture2D> pGameDepthBufferQuarter;
    ComPtr<ID3D11Texture2D> pGameDepthBufferResolved;
    ComPtr<ID3D11Texture2D> pGameDepthBuffer;
    ComPtr<ID3D11Texture2D> pGameGBuffer0;
    ComPtr<ID3D11Texture2D> pGameEdgeCopy;
    ComPtr<ID3D11Texture2D> pLinearDepthTexture;
    ComPtr<ID3D11Texture2D> pStencilTexture;
    void* pCtxLinearizeBuffer = nullptr;
    bool isCustomFrameRateSupported = false;
    bool isEnhancedDualPassSupported = false;
    std::unique_ptr<Encoder::EncoderSession> encodingSession;
    std::mutex mxSession;
    std::mutex mxOnPresent;
    std::condition_variable mainSwapChainReadyCv;
    std::thread::id mainThreadId;
    ComPtr<IDXGISwapChain> mainSwapChain;
    ComPtr<IDXGIFactory> pDxgiFactory;
    ComPtr<ID3D11DeviceContext> pDContext;
    ComPtr<ID3D11Texture2D> pMotionBlurAccBuffer;
    ComPtr<ID3D11Texture2D> pMotionBlurFinalBuffer;
    ComPtr<ID3D11VertexShader> pVsFullScreen;
    ComPtr<ID3D11PixelShader> pPsAccumulate;
    ComPtr<ID3D11PixelShader> pPsDivide;
    ComPtr<ID3D11Buffer> pDivideConstantBuffer;
    ComPtr<ID3D11RasterizerState> pRasterState;
    ComPtr<ID3D11SamplerState> pPsSampler;
    ComPtr<ID3D11RenderTargetView> pRtvAccBuffer;
    ComPtr<ID3D11RenderTargetView> pRtvBlurBuffer;
    ComPtr<ID3D11ShaderResourceView> pSrvAccBuffer;
    ComPtr<ID3D11Texture2D> pSourceSrvTexture;
    ComPtr<ID3D11ShaderResourceView> pSourceSrv;
    ComPtr<ID3D11BlendState> pAccBlendState;
    std::mutex d3d11HookMutex;
    bool isD3D11HookInstalled = false;
    thread_local bool isInstallingD3D11Hook = false;
    std::atomic_bool hasLoggedD3D11HookFailure = false;
    std::atomic_bool hasLoggedMissingFactory2 = false;
    std::chrono::steady_clock::time_point lastD3D11HookAttempt;
    std::mutex reshadeCaptureMutex;
    std::vector<uint8_t> reshadeCapturePixels;
    std::atomic_bool hasLoggedEnhancedCaptureResolutionMismatch = false;
    std::atomic_bool hasLoggedEnhancedCaptureSuccess = false;
    std::atomic_bool hasLoggedEnhancedNamedCaptureFailure = false;
    std::atomic_uint32_t enhancedPendingVideoFrames = 0;
    std::atomic_uint32_t enhancedExpectedWidth = 0;
    std::atomic_uint32_t enhancedExpectedHeight = 0;
    std::atomic_uint32_t enhancedRuntimeWidth = 0;
    std::atomic_uint32_t enhancedRuntimeHeight = 0;
    std::atomic<reshade::api::effect_runtime*> enhancedRuntimeSource = nullptr;
    std::atomic_bool enhancedVideoResourceTracked = false;
    std::mutex enhancedResourceMutex;
    reshade::api::resource enhancedVideoEncodeResource{};
    reshade::api::resource_usage enhancedVideoEncodeState = reshade::api::resource_usage::undefined;
    reshade::api::device* enhancedCleanCandidateDevice = nullptr;
    reshade::api::resource enhancedCleanCandidateResource{};
    reshade::api::resource_usage enhancedCleanCandidateState = reshade::api::resource_usage::undefined;
    reshade::api::device* enhancedDepthCandidateDevice = nullptr;
    reshade::api::resource enhancedDepthCandidateResource{};
    reshade::api::resource_usage enhancedDepthCandidateState = reshade::api::resource_usage::undefined;

    constexpr size_t kEnhancedReadbackSlotCount = 3;
    struct EnhancedReadbackSlot {
        reshade::api::resource resource{};
        reshade::api::resource depthResource{};
        reshade::api::fence fence{};
        uint64_t fenceValue = 0;
        uint64_t generation = 0;
        bool gpuMotionBlurOutput = false;
        bool pending = false;
    };
    std::array<EnhancedReadbackSlot, kEnhancedReadbackSlotCount> enhancedReadbackSlots{};
    std::deque<size_t> enhancedReadbackOrder;
    size_t enhancedReadbackCursor = 0;
    reshade::api::effect_runtime* enhancedReadbackRuntime = nullptr;
    reshade::api::device* enhancedReadbackDevice = nullptr;
    uint32_t enhancedReadbackWidth = 0;
    uint32_t enhancedReadbackHeight = 0;
    reshade::api::format enhancedReadbackFormat = reshade::api::format::unknown;
    reshade::api::format enhancedReadbackDepthSourceFormat = reshade::api::format::unknown;
    reshade::api::format enhancedReadbackDepthFormat = reshade::api::format::unknown;
    bool enhancedReadbackHasDepth = false;
    std::atomic_uint64_t enhancedCaptureGeneration = 1;
    std::mutex enhancedReadbackMutex;
    std::mutex enhancedDiagnosticMutex;
    std::unordered_set<uint64_t> enhancedBackBufferHandles;
    std::unordered_set<uint64_t> enhancedLoggedRenderTargetHandles;
    std::unordered_set<uint64_t> enhancedLoggedDepthStencilHandles;
    std::unordered_set<uint64_t> enhancedLoggedCopySourceHandles;
    std::atomic_bool hasLoggedEnhancedBackBuffer = false;
    std::atomic_bool hasLoggedEnhancedCleanCapture = false;
    std::atomic_bool enhancedPostEffectsCaptureReady = false;
    std::atomic_uint64_t enhancedPostEffectsBackBufferHandle = 0;
    std::atomic_bool hasLoggedEnhancedPreEffectsReplacement = false;
    std::atomic_bool hasLoggedEnhancedMissingDepth = false;
    std::atomic_bool enhancedOpenExrPayloadValidated = false;
    std::vector<float> enhancedCaptureDepth;

    struct EnhancedMotionBlurState {
        std::vector<uint16_t> accumulation;
        std::vector<uint8_t> output;
        uint32_t rowPitch = 0;
        uint32_t height = 0;
        uint32_t subframeIndex = 0;
        uint32_t accumulatedSamples = 0;
        uint8_t configuredSamples = 0;
        float configuredStrength = 0.0f;
        uint64_t consumedSubframes = 0;
        uint64_t emittedFrames = 0;
    };
    EnhancedMotionBlurState enhancedMotionBlur;

    struct EnhancedGpuMotionBlurState {
        reshade::api::effect_runtime* runtime = nullptr;
        reshade::api::device* device = nullptr;
        reshade::api::resource sourceCopy{};
        reshade::api::resource accumulation{};
        reshade::api::resource finalOutput{};
        reshade::api::resource_view sourceSrv{};
        reshade::api::resource_view accumulationSrv{};
        reshade::api::resource_view accumulationRtv{};
        reshade::api::resource_view finalRtv{};
        reshade::api::pipeline_layout pipelineLayout{};
        reshade::api::pipeline accumulatePipeline{};
        reshade::api::pipeline dividePipeline{};
        uint32_t width = 0;
        uint32_t height = 0;
        reshade::api::format sourceFormat = reshade::api::format::unknown;
        uint32_t subframeIndex = 0;
        uint32_t accumulatedSamples = 0;
        uint8_t configuredSamples = 0;
        float configuredStrength = 0.0f;
        uint64_t consumedSubframes = 0;
        uint64_t emittedFrames = 0;
        bool creationFailed = false;
    };
    EnhancedGpuMotionBlurState enhancedGpuMotionBlur;

    std::string outputExtensionForContainer(const char* container) {
        const std::string format = (container && *container) ? container : "mp4";
        if (format == "matroska" || format == "mkv") {
            return ".mkv";
        }
        return "." + format;
    }

    template <typename Function>
    void parallelForMotionBlurBytes(const size_t byteCount, Function&& function) {
        constexpr size_t kChunkBytes = 1u << 20;
        const size_t chunkCount = (byteCount + kChunkBytes - 1) / kChunkBytes;
        if (chunkCount <= 1) {
            function(0, byteCount);
            return;
        }
        Concurrency::parallel_for<size_t>(0, chunkCount, [&](const size_t chunkIndex) {
            const size_t begin = chunkIndex * kChunkBytes;
            const size_t end = std::min(begin + kChunkBytes, byteCount);
            function(begin, end);
        });
    }

    constexpr std::chrono::milliseconds kD3D11HookRetryDelay(100);
    constexpr std::chrono::milliseconds kSwapChainReadyTimeout(5000);
    constexpr std::chrono::milliseconds kSwapChainReadyPollInterval(250);

    constexpr SIZE_T kMaxLoggedAnsiChars = 1024;

    std::string SafeAnsiString(const char* value) {
        if (!value) {
            return "<NULL>";
        }

        if (IsBadStringPtrA(value, static_cast<UINT_PTR>(kMaxLoggedAnsiChars))) {
            std::ostringstream oss;
            oss << "<INVALID PTR 0x" << std::uppercase << std::hex << reinterpret_cast<uintptr_t>(value) << ">";
            return oss.str();
        }

        const size_t len = strnlen_s(value, kMaxLoggedAnsiChars);
        std::string result(value, len);
        if (len == kMaxLoggedAnsiChars) {
            result.append("...");
        }
        return result;
    }

    bool IsTrackedCreateTextureName(const std::string& name) {
        return name == "DepthBuffer_Resolved" ||
               name == "DepthBufferCopy" ||
               name == "DepthBuffer" ||
               name == "Depth Quarter" ||
               name == "Depth Quarter Linear" ||
               name == "GBUFFER_0" ||
               name == "Edge Copy" ||
               name == "BackBuffer" ||
               name == "BackBuffer_Resolved" ||
               name == "BackBufferCopy" ||
               name == "VideoEncode";
    }

    bool IsRunningInFiveMRuntime() {
        static const bool isFiveM =
            GetModuleHandleW(L"citizen-game-main.dll") != nullptr ||
            GetModuleHandleW(L"asi-five.dll") != nullptr ||
            GetModuleHandleW(L"scripting-gta.dll") != nullptr;
        return isFiveM;
    }

    bool DetectEnhancedEdition() {
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return false;
        }

        const std::wstring fullPath(path.data(), length);
        const size_t separator = fullPath.find_last_of(L"\\/");
        const std::wstring fileName =
            separator == std::wstring::npos ? fullPath : fullPath.substr(separator + 1);
        return _wcsicmp(fileName.c_str(), L"GTA5_Enhanced.exe") == 0;
    }

    const bool g_isEnhancedEdition = DetectEnhancedEdition();

    class ScopedFlagGuard {
    public:
        explicit ScopedFlagGuard(bool& flag) : ref(flag) { ref = true; }

        ScopedFlagGuard(const ScopedFlagGuard&) = delete;
        ScopedFlagGuard& operator=(const ScopedFlagGuard&) = delete;

    private:
        bool& ref;
    };

    struct ExportContext {
        ExportContext() {
            PRE();
            POST();
        }
        ~ExportContext() {
            PRE();
            POST();
        }

        ExportContext(const ExportContext& other) = delete;
        ExportContext& operator=(const ExportContext& other) = delete;
        ExportContext(ExportContext&& other) = delete;
        ExportContext& operator=(ExportContext&& other) = delete;

        bool is_audio_export_disabled = false;
        ComPtr<ID3D11Texture2D> p_export_render_target;
        ComPtr<ID3D11DeviceContext> p_device_context;
        ComPtr<ID3D11Device> p_device;
        ComPtr<IDXGISwapChain> p_swap_chain;
        ComPtr<IMFMediaType> video_media_type;
        DWORD video_stream_index = 0;
        DWORD audio_stream_index = 1;
        int acc_count = 0;
        int total_frame_num = 0;
    };

    std::unique_ptr<ExportContext> exportContext;
    std::unique_ptr<ever::hooking::PatternScanner> patternScanner;

    enum class DualPassState {
        IDLE,
        PASS1_RUNNING,
        PASS1_COMPLETE,
        PASS2_PENDING,
        PASS2_RUNNING,
        PASS2_COMPLETE 
    };

    struct DualPassContext {
        DualPassState state = DualPassState::IDLE;
        void* saved_video_editor_interface = nullptr;
        void* saved_montage_ptr = nullptr;
        std::string enhanced_export_name;
        bool enhanced_export_name_present = false;
        
        std::pair<int32_t, int32_t> original_fps;
        int32_t original_motion_blur_samples = 0;
        
        std::string timestamp;
        std::string final_output_file;
        
        std::vector<BYTE> audio_buffer;
        uint32_t audio_sample_rate = 48000;
        uint16_t audio_channels = 2;
        uint16_t audio_bits_per_sample = 16;
        uint32_t audio_block_align = 4;
        size_t audio_buffer_playback_position = 0;
    };

    std::unique_ptr<DualPassContext> dualPassContext;
    std::atomic_bool ffmpegExportFailed = false;
    std::mutex ffmpegExportFailureMutex;
    std::string ffmpegExportFailureStage;
    std::atomic_bool asyncFinalizeInProgress = false;
    std::atomic_bool asyncFinalizeCompleted = false;
    std::atomic_bool asyncFinalizeFailed = false;

    void markFfmpegExportFailure(const char* stage, const HRESULT hr = E_FAIL) {
        ffmpegExportFailed = true;
        std::ostringstream oss;
        oss << stage << " hr=" << Logger::hex(static_cast<uint32_t>(hr), 8);
        {
            std::lock_guard<std::mutex> lock(ffmpegExportFailureMutex);
            ffmpegExportFailureStage = oss.str();
        }
        LOG(LL_ERR, "FFmpeg export marked as failed at stage: ", ffmpegExportFailureStage);
    }

    void clearFfmpegExportFailure() {
        ffmpegExportFailed = false;
        std::lock_guard<std::mutex> lock(ffmpegExportFailureMutex);
        ffmpegExportFailureStage.clear();
    }

    std::string getFfmpegExportFailureStage() {
        std::lock_guard<std::mutex> lock(ffmpegExportFailureMutex);
        return ffmpegExportFailureStage;
    }

    void clearAsyncFinalizeState() {
        asyncFinalizeInProgress = false;
        asyncFinalizeCompleted = false;
        asyncFinalizeFailed = false;
    }

    bool startAsyncEncoderFinalization(std::unique_ptr<Encoder::EncoderSession> session,
                                       const std::string& stagePrefix) {
        if (!session) {
            LOG(LL_ERR, "Cannot start async FFmpeg finalization without an encoding session");
            asyncFinalizeFailed = true;
            asyncFinalizeCompleted = true;
            asyncFinalizeInProgress = false;
            markFfmpegExportFailure((stagePrefix + "::missing-session").c_str(), E_POINTER);
            return false;
        }

        asyncFinalizeInProgress = true;
        asyncFinalizeCompleted = false;
        asyncFinalizeFailed = false;

        try {
            std::thread([session = std::move(session), stagePrefix]() mutable {
                bool workerSuccess = true;
                try {
                    LOG(LL_NFO, "Async FFmpeg finalization thread started. context=", stagePrefix);

                    const HRESULT finishAudioHr = session->finishAudio();
                    if (FAILED(finishAudioHr)) {
                        workerSuccess = false;
                        markFfmpegExportFailure((stagePrefix + "::finishAudio").c_str(), finishAudioHr);
                    }

                    const HRESULT finishVideoHr = session->finishVideo();
                    if (FAILED(finishVideoHr)) {
                        workerSuccess = false;
                        markFfmpegExportFailure((stagePrefix + "::finishVideo").c_str(), finishVideoHr);
                    }

                    const HRESULT endSessionHr = session->endSession();
                    if (FAILED(endSessionHr)) {
                        workerSuccess = false;
                        markFfmpegExportFailure((stagePrefix + "::endSession").c_str(), endSessionHr);
                    }
                } catch (const std::exception& ex) {
                    workerSuccess = false;
                    LOG(LL_ERR, "Async FFmpeg finalization exception: ", ex.what());
                    markFfmpegExportFailure((stagePrefix + "::exception").c_str(), E_FAIL);
                } catch (...) {
                    workerSuccess = false;
                    LOG(LL_ERR, "Async FFmpeg finalization unknown exception");
                    markFfmpegExportFailure((stagePrefix + "::unknown").c_str(), E_FAIL);
                }

                session.reset();

                asyncFinalizeFailed = !workerSuccess;
                asyncFinalizeCompleted = true;
                asyncFinalizeInProgress = false;

                if (workerSuccess) {
                    clearFfmpegExportFailure();
                    LOG(LL_NFO, "Async FFmpeg finalization completed successfully. context=", stagePrefix);
                } else {
                    LOG(LL_ERR, "Async FFmpeg finalization failed. stage=", getFfmpegExportFailureStage());
                }
            }).detach();
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "Failed to start async FFmpeg finalization thread: ", ex.what());
            asyncFinalizeFailed = true;
            asyncFinalizeCompleted = true;
            asyncFinalizeInProgress = false;
            markFfmpegExportFailure((stagePrefix + "::thread-start").c_str(), E_FAIL);
            return false;
        } catch (...) {
            LOG(LL_ERR, "Failed to start async FFmpeg finalization thread (unknown exception)");
            asyncFinalizeFailed = true;
            asyncFinalizeCompleted = true;
            asyncFinalizeInProgress = false;
            markFfmpegExportFailure((stagePrefix + "::thread-start-unknown").c_str(), E_FAIL);
            return false;
        }

        return true;
    }

    // Manual hooks for functions that can't use PolyHook
    using CleanupReplayPlaybackInternalFunc = void(*)();
    CleanupReplayPlaybackInternalFunc pCleanupReplayPlaybackOriginal = nullptr;
    void* pCleanupReplayPlaybackTrampoline = nullptr;
    uint8_t g_cleanupOriginalBytes[12] = {0}; // Store original bytes for safe unhook/rehook
    
    // KillPlaybackOrBake hook (manual hook with unhook/rehook pattern)
    using KillPlaybackOrBakeFunc = void(*)(void* thisPtr, bool userCancelled);
    KillPlaybackOrBakeFunc pKillPlaybackOrBakeOriginal = nullptr;
    uint8_t g_killPlaybackOrBakeOriginalBytes[12] = {0};
    using EnhancedKillPlaybackOrBakeFunc = void(*)(bool userCancelled);
    EnhancedKillPlaybackOrBakeFunc pEnhancedKillPlaybackOrBakeOriginal = nullptr;
    uint8_t g_enhancedKillPlaybackOrBakeOriginalBytes[12] = {0};
    uint64_t g_enhancedKillPass1ExitPatchAddress = 0;
    void* g_enhancedKillPass1ExitStub = nullptr;

    // SetUserConfirmationScreen hook (manual hook with unhook/rehook pattern)
    using SetUserConfirmationScreenFunc = void(*)(const char* pTextLabel,
                                                 const char* pTitle,
                                                 uint32_t type,
                                                 bool allowSpinner,
                                                 const char* literalString,
                                                 const char* literalString2);
    SetUserConfirmationScreenFunc pSetUserConfirmationScreenOriginal = nullptr;
    uint8_t g_setUserConfirmationScreenOriginalBytes[12] = {0};

    std::atomic_bool g_pass2TriggerIssued = false;
    std::atomic_bool g_pass2StartupPending = false;
    std::atomic_int g_pass2StartupDelayFrames = 0;
    constexpr int kPass2StartupDelayFrames = 60;
    uint8_t* g_wantDelayedClosePtr = nullptr;
    thread_local bool g_enhancedDualPassCleanupInProgress = false;
    thread_local bool g_enhancedIsPendingHandoffInProgress = false;
    
    // Store IsPendingBakeStart address for hook to access
    uint64_t g_isPendingBakeStartAddress = 0;
    uint32_t* g_isPendingBakeStartStatePtr = nullptr;

    void schedulePass2StartupOnMainThread(const char* reason);
    bool executePass2StartupOnMainThread();
    bool callEnhancedCleanupReplayPlaybackOriginal();
    bool callEnhancedKillPlaybackOrBakeOriginal(bool userCancelled);
    bool prepareEnhancedKillPass1ExitPatch(uint64_t functionAddress);
    bool callEnhancedKillPlaybackOrBakeThroughFinalize();

    struct TemporalSyncState {
        double ripplesAccumulatorSec = 0.0;
        uint64_t ripplesCalls = 0;
        uint64_t ripplesSkipped = 0;
        double clothAccumulatorSec = 0.0;
        uint64_t clothCalls = 0;
        uint64_t clothSkipped = 0;
        bool wasExportActive = false;
    };

    std::mutex g_temporalSyncMutex;
    TemporalSyncState g_temporalSyncState;
    std::atomic_bool g_loggedRipplesExportHit = false;
    std::atomic_bool g_loggedClothExportHit = false;
    // Pointer to rage::fwTimer::sm_systemTime.m_StepInSeconds
    float* g_pClothSystemTimeStep = nullptr;

    constexpr double kTemporalSyncTargetStepSec = 1.0 / 30.0;
    constexpr double kTemporalSyncMaxAccumulatorSec = 0.25;

    bool bindExportSwapChainIfAvailableLocked() {
        if (!exportContext) {
            return false;
        }

        if (exportContext->p_swap_chain) {
            return true;
        }

        if (!mainSwapChain) {
            return false;
        }

        exportContext->p_swap_chain = mainSwapChain;
        LOG(LL_DBG, "Export context bound to main swap chain",
            Logger::hex(reinterpret_cast<uint64_t>(mainSwapChain.Get()), 16));
        return true;
    }

    bool bindExportSwapChainIfAvailable() {
        std::lock_guard onPresentLock(mxOnPresent);
        return bindExportSwapChainIfAvailableLocked();
    }

    bool waitForMainSwapChain(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mxOnPresent);
        if (mainSwapChain) {
            LOG(LL_DBG, "Swap chain already available before wait started");
            return true;
        }

        if (timeout.count() <= 0) {
            LOG(LL_DBG, "Swap chain wait requested with non-positive timeout; skipping wait");
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        auto remaining = timeout;
        size_t attempt = 0;

        while (remaining.count() > 0 && !mainSwapChain) {
            const auto slice = std::min(remaining, kSwapChainReadyPollInterval);
            ++attempt;
            if (mainSwapChainReadyCv.wait_for(lock, slice, [] { return mainSwapChain != nullptr; })) {
                const auto waited =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                LOG(LL_DBG, "Swap chain became ready after ", waited.count(), "ms (", attempt, " wait cycles)");
                return true;
            }

            remaining -= slice;
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(timeout - remaining).count();
            LOG(LL_DBG, "Swap chain still pending after ", elapsed, "ms (attempt ", attempt, ")");
        }

        LOG(LL_DBG, "Swap chain wait timed out after ", timeout.count(), "ms");
        return mainSwapChain != nullptr;
    }

    void captureMainSwapChain(IDXGISwapChain* pSwapChain, const char* sourceLabel) {
        if (!pSwapChain) {
            LOG(LL_DBG, sourceLabel, ": swap chain pointer was null");
            return;
        }

        std::lock_guard onPresentLock(mxOnPresent);
        const bool isSame = (mainSwapChain.Get() == pSwapChain);
        mainSwapChain = pSwapChain;
        mainSwapChainReadyCv.notify_all();
        bindExportSwapChainIfAvailableLocked();

        if (isSame) {
            LOG(LL_TRC, sourceLabel, ": reuse of existing swap chain ptr:",
                Logger::hex(reinterpret_cast<uint64_t>(pSwapChain), 16));
        } else {
            LOG(LL_DBG, sourceLabel, ": captured swap chain ptr:",
                Logger::hex(reinterpret_cast<uint64_t>(pSwapChain), 16));
        }
    }

    void installFactoryHooks(IDXGIFactory* factory) {
        if (!factory) {
            return;
        }

        {
            std::lock_guard onPresentLock(mxOnPresent);
            if (!pDxgiFactory || (pDxgiFactory.Get() != factory)) {
                pDxgiFactory = factory;
            }
        }

        try {
            PERFORM_MEMBER_HOOK_REQUIRED(IDXGIFactory, CreateSwapChain, factory);
        } catch (const std::exception& ex) {
            LOG(LL_WRN, "Failed to hook IDXGIFactory::CreateSwapChain: ", ex.what());
        } catch (...) {
            LOG(LL_WRN, "Failed to hook IDXGIFactory::CreateSwapChain");
        }

        ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(factory->QueryInterface(factory2.GetAddressOf()))) {
            try {
                PERFORM_MEMBER_HOOK_REQUIRED(IDXGIFactory2, CreateSwapChainForHwnd, factory2.Get());
            } catch (const std::exception& ex) {
                LOG(LL_WRN, "Failed to hook IDXGIFactory2::CreateSwapChainForHwnd: ", ex.what());
            } catch (...) {
                LOG(LL_WRN, "Failed to hook IDXGIFactory2::CreateSwapChainForHwnd");
            }

            try {
                PERFORM_MEMBER_HOOK_REQUIRED(IDXGIFactory2, CreateSwapChainForCoreWindow, factory2.Get());
            } catch (const std::exception& ex) {
                LOG(LL_WRN, "Failed to hook IDXGIFactory2::CreateSwapChainForCoreWindow: ", ex.what());
            } catch (...) {
                LOG(LL_WRN, "Failed to hook IDXGIFactory2::CreateSwapChainForCoreWindow");
            }

            try {
                PERFORM_MEMBER_HOOK_REQUIRED(IDXGIFactory2, CreateSwapChainForComposition, factory2.Get());
            } catch (const std::exception& ex) {
                LOG(LL_WRN, "Failed to hook IDXGIFactory2::CreateSwapChainForComposition: ", ex.what());
            } catch (...) {
                LOG(LL_WRN, "Failed to hook IDXGIFactory2::CreateSwapChainForComposition");
            }
        } else if (!hasLoggedMissingFactory2.exchange(true)) {
            LOG(LL_DBG, "IDXGIFactory does not expose IDXGIFactory2 interfaces; skipping extended hooks");
        }
    }

    void installFactoryHooksFromSwapChain(IDXGISwapChain* swapChain, const char* sourceLabel) {
        if (!swapChain) {
            return;
        }

        ComPtr<IDXGIFactory> swapFactory;
        if (FAILED(swapChain->GetParent(__uuidof(IDXGIFactory),
                                        reinterpret_cast<void**>(swapFactory.GetAddressOf())))) {
            LOG(LL_DBG, sourceLabel, ": failed to query IDXGIFactory from swap chain");
            return;
        }

        installFactoryHooks(swapFactory.Get());
    }

    void captureAndHookSwapChain(IDXGISwapChain* swapChain, const char* sourceLabel) {
        if (!swapChain) {
            LOG(LL_DBG, sourceLabel, ": capture request received null swap chain pointer");
            return;
        }

        captureMainSwapChain(swapChain, sourceLabel);
        try {
            PERFORM_MEMBER_HOOK_REQUIRED(IDXGISwapChain, Present, swapChain);
        } catch (...) {
            LOG(LL_ERR, sourceLabel, ": failed to hook IDXGISwapChain::Present");
        }
    }

    void captureSwapChainFromSwapChain1(IDXGISwapChain1* swapChain, const char* sourceLabel) {
        if (!swapChain) {
            LOG(LL_DBG, sourceLabel, ": IDXGISwapChain1 pointer was null");
            return;
        }

        ComPtr<IDXGISwapChain> baseSwapChain;
        if (FAILED(swapChain->QueryInterface(baseSwapChain.GetAddressOf()))) {
            LOG(LL_WRN, sourceLabel, ": failed to query IDXGISwapChain from IDXGISwapChain1");
            return;
        }

        captureAndHookSwapChain(baseSwapChain.Get(), sourceLabel);
    }

    std::string dumpMemoryBytes(uint64_t address, const size_t byteCount) {
        if (address == 0 || byteCount == 0) {
            return {};
        }

        std::vector<uint8_t> buffer(byteCount, 0);
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer.data(), byteCount,
                            &bytesRead)) {
            const auto err = GetLastError();
            LOG(LL_WRN, "ReadProcessMemory failed for address", Logger::hex(address, 16), ": ", err, " ",
                Logger::hex(err, 8));
            return {};
        }

        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (SIZE_T i = 0; i < bytesRead; ++i) {
            if (i != 0) {
                stream << ' ';
            }
            stream << std::setw(2) << static_cast<uint32_t>(buffer[i]);
        }

        return stream.str();
    }

    bool memoryStartsWith(const uint64_t address, std::initializer_list<uint8_t> expectedPrefix) {
        if (address == 0 || expectedPrefix.size() == 0) {
            return false;
        }

        std::vector<uint8_t> buffer(expectedPrefix.size(), 0);
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
                               buffer.data(), buffer.size(), &bytesRead)) {
            return false;
        }

        if (bytesRead < expectedPrefix.size()) {
            return false;
        }

        size_t idx = 0;
        for (const uint8_t value : expectedPrefix) {
            if (buffer[idx] != value) {
                return false;
            }
            ++idx;
        }

        return true;
    }

    uint64_t findNearestBackwardMatch(
        const uint64_t candidateAddress,
        const size_t maxBacktrack,
        std::initializer_list<uint8_t> signature) {
        if (candidateAddress == 0 || signature.size() == 0) {
            return 0;
        }

        for (size_t back = 0; back <= maxBacktrack; ++back) {
            const uint64_t probe = candidateAddress - back;
            if (memoryStartsWith(probe, signature)) {
                return probe;
            }

            if (probe == 0) {
                break;
            }
        }

        return 0;
    }

    // TODO: This can most likely be imporoved or have a more stable pattern.
    uint64_t resolvePuddlesRipplesUpdateFunction(uint64_t candidateAddress) {
        if (candidateAddress == 0) {
            return 0;
        }

        if (memoryStartsWith(candidateAddress, {0x53, 0x48, 0x83, 0xEC, 0x60})) {
            return candidateAddress;
        }

        if (memoryStartsWith(candidateAddress, {0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05}) &&
            candidateAddress > 0 &&
            memoryStartsWith(candidateAddress - 1, {0x53, 0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05})) {
            return candidateAddress - 1;
        }

        if (memoryStartsWith(candidateAddress, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60})) {
            return candidateAddress;
        }

        if (const uint64_t fixed = findNearestBackwardMatch(
                candidateAddress,
                0x200,
                {0x53, 0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05});
            fixed != 0) {
            return fixed;
        }

        if (const uint64_t fixed = findNearestBackwardMatch(
                candidateAddress,
                0x200,
                {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05});
            fixed != 0) {
            return fixed;
        }

        if (const uint64_t fixed = findNearestBackwardMatch(
                candidateAddress,
                0x200,
                {0x48, 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05});
            fixed != 0) {
            return fixed;
        }

        return candidateAddress;
    }

    void logResolvedHookTarget(const char* label, const uint64_t address, const size_t previewBytes = 32) {
        if (address == 0) {
            LOG(LL_WRN, label, " address was not resolved");
            return;
        }

        LOG(LL_NFO, label, " address:", Logger::hex(address, 16));
        const auto bytes = dumpMemoryBytes(address, previewBytes);
        if (!bytes.empty()) {
            LOG(LL_DBG, label, " first bytes:", bytes);
        }
    }

    const char* dualPassStateName(DualPassState state) {
        switch (state) {
            case DualPassState::IDLE: return "IDLE";
            case DualPassState::PASS1_RUNNING: return "PASS1_RUNNING";
            case DualPassState::PASS1_COMPLETE: return "PASS1_COMPLETE";
            case DualPassState::PASS2_PENDING: return "PASS2_PENDING";
            case DualPassState::PASS2_RUNNING: return "PASS2_RUNNING";
            case DualPassState::PASS2_COMPLETE: return "PASS2_COMPLETE";
            default: return "UNKNOWN";
        }
    }

    bool isPass1SuccessLabel(const char* pTextLabel) {
        if (!pTextLabel) {
            return false;
        }

        return std::strcmp(pTextLabel, "VE_BAKE_FIN") == 0 ||
               std::strcmp(pTextLabel, "VE_BAKE_FIN_NAMED") == 0;
    }

    bool isPass1TransitionSuppressLabel(const char* pTextLabel) {
        if (!pTextLabel) {
            return false;
        }

        return isPass1SuccessLabel(pTextLabel) ||
               std::strcmp(pTextLabel, "VE_BAKE_ERROR") == 0;
    }

    bool isExportCompletionLabel(const char* pTextLabel) {
        if (!pTextLabel) {
            return false;
        }

        return std::strcmp(pTextLabel, "VE_BAKE_FIN") == 0 ||
               std::strcmp(pTextLabel, "VE_BAKE_FIN_NAMED") == 0;
    }

    bool isDualPassTransitionState() {
        if (!dualPassContext) {
            return false;
        }

        return dualPassContext->state == DualPassState::PASS1_COMPLETE ||
               dualPassContext->state == DualPassState::PASS2_PENDING;
    }

    void tryForceWantDelayedCloseFalse(const char* reason) {
        if (!g_wantDelayedClosePtr) {
            return;
        }

        if (!isDualPassTransitionState()) {
            return;
        }

        if (*g_wantDelayedClosePtr) {
            *g_wantDelayedClosePtr = 0;
            LOG(LL_NFO, "Forced ms_bWantDelayedClose=false during dual-pass transition. reason=", reason,
                " state=", dualPassContext ? dualPassStateName(dualPassContext->state) : "NO_CONTEXT");
        }
    }

    double getCurrentExportRenderStepSeconds() {
        const auto fps = Config::Manager::fps;
        const float sampleCount = static_cast<float>(Config::Manager::motion_blur_samples) + 1.0f;
        const float denominator = static_cast<float>(fps.first) * sampleCount;

        if (denominator <= 0.0f) {
            return kTemporalSyncTargetStepSec;
        }

        const float milliseconds = 1000.0f * static_cast<float>(fps.second) / denominator;
        return std::max(0.0001, static_cast<double>(milliseconds) / 1000.0);
    }

    bool shouldExecuteTemporalTick(double& accumulatorSec, const double deltaSec) {
        accumulatorSec = std::min(accumulatorSec + deltaSec, kTemporalSyncMaxAccumulatorSec);
        if (accumulatorSec + 1e-9 < kTemporalSyncTargetStepSec) {
            return false;
        }

        accumulatorSec = std::fmod(accumulatorSec, kTemporalSyncTargetStepSec);
        return true;
    }

    void resetTemporalSyncStateLocked() {
        g_temporalSyncState.ripplesAccumulatorSec = 0.0;
        g_temporalSyncState.ripplesCalls = 0;
        g_temporalSyncState.ripplesSkipped = 0;
        g_temporalSyncState.clothAccumulatorSec = 0.0;
        g_temporalSyncState.clothCalls = 0;
        g_temporalSyncState.clothSkipped = 0;
    }

    void updateTemporalSyncExportTransitionLocked(const bool exportActive) {
        if (g_temporalSyncState.wasExportActive == exportActive) {
            return;
        }

        g_temporalSyncState.wasExportActive = exportActive;
        resetTemporalSyncStateLocked();
        LOG(LL_NFO, "Temporal sync state transition: exportActive=", exportActive);
    }

    void logTemporalSyncCounters(const char* label, const uint64_t calls, const uint64_t skipped, const double stepSec) {
        if (calls == 0 || (calls % 240) != 0) {
            return;
        }

        LOG(LL_DBG, "Temporal sync ", label,
            " calls=", calls,
            " skipped=", skipped,
            " stepMs=", static_cast<float>(stepSec * 1000.0));
    }

    void resolveWantDelayedCloseAddress(const uint64_t setUserConfirmationAddress) {
        if (setUserConfirmationAddress == 0) {
            return;
        }

        MODULEINFO moduleInfo{};
        if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &moduleInfo, sizeof(moduleInfo))) {
            LOG(LL_WRN, "resolveWantDelayedCloseAddress: GetModuleInformation failed");
            return;
        }

        const auto imageStart = reinterpret_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
        const auto imageSize = static_cast<size_t>(moduleInfo.SizeOfImage);

        for (size_t offset = 0; offset + 12 < imageSize; ++offset) {
            uint8_t* p = imageStart + offset;

            if (p[0] != 0xE8 || p[5] != 0xC6 || p[6] != 0x05 || p[11] != 0x01) {
                continue;
            }

            const auto relCall = *reinterpret_cast<int32_t*>(p + 1);
            const auto callTarget = reinterpret_cast<uint64_t>(p + 5 + relCall);
            if (callTarget != setUserConfirmationAddress) {
                continue;
            }

            const auto relStore = *reinterpret_cast<int32_t*>(p + 7);
            uint8_t* candidate = p + 12 + relStore;

            if (candidate < imageStart || candidate >= imageStart + imageSize) {
                continue;
            }

            g_wantDelayedClosePtr = candidate;
            LOG(LL_NFO, "ms_bWantDelayedClose candidate resolved at ",
                Logger::hex(reinterpret_cast<uint64_t>(g_wantDelayedClosePtr), 16),
                " (call->SetUserConfirmation+store pattern)");
            return;
        }

        LOG(LL_WRN, "Could not resolve ms_bWantDelayedClose address from SetUserConfirmation call pattern");
    }

    bool installAbsoluteJumpHook(uint64_t address, uintptr_t hookAddress) {
        if (address == 0 || hookAddress == 0) {
            return false;
        }

        constexpr size_t patchSize = 12; // mov rax, imm64; jmp rax
        std::array<uint8_t, patchSize> patch{};
        patch[0] = 0x48;
        patch[1] = 0xB8;

        std::memcpy(&patch[2], &hookAddress, sizeof(hookAddress));
        patch[10] = 0xFF;
        patch[11] = 0xE0;

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(address), patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            const auto err = GetLastError();
            LOG(LL_ERR, "VirtualProtect failed for", Logger::hex(address, 16), ": ", err, " ",
                Logger::hex(err, 8));
            return false;
        }

        std::memcpy(reinterpret_cast<void*>(address), patch.data(), patchSize);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), patchSize);

        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(address), patchSize, oldProtect, &unused);
        LOG(LL_DBG, "Installed absolute jump hook at", Logger::hex(address, 16), " -> ",
            Logger::hex(static_cast<uint64_t>(hookAddress), 16));
        return true;
    }

    bool callEnhancedCleanupReplayPlaybackOriginal() {
        const uint64_t targetAddress =
            reinterpret_cast<uint64_t>(pCleanupReplayPlaybackOriginal);
        if (targetAddress == 0) {
            LOG(LL_ERR, "Enhanced CleanupReplayPlayback original target is null");
            return false;
        }

        constexpr size_t patchSize = sizeof(g_cleanupOriginalBytes);
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(targetAddress), patchSize,
                            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG(LL_ERR, "Failed to restore Enhanced CleanupReplayPlayback bytes. error=",
                GetLastError());
            return false;
        }

        std::memcpy(reinterpret_cast<void*>(targetAddress), g_cleanupOriginalBytes, patchSize);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(targetAddress), patchSize);
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(targetAddress), patchSize, oldProtect, &unused);

        bool originalCallSucceeded = true;
        try {
            reinterpret_cast<CleanupReplayPlaybackInternalFunc>(targetAddress)();
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "Enhanced CleanupReplayPlayback original threw: ", ex.what());
            originalCallSucceeded = false;
        } catch (...) {
            LOG(LL_ERR, "Enhanced CleanupReplayPlayback original threw an unknown exception");
            originalCallSucceeded = false;
        }

        const bool hookRestored = installAbsoluteJumpHook(
            targetAddress,
            reinterpret_cast<uintptr_t>(GameHooks::EnhancedCleanupReplayPlayback::Implementation));
        if (!hookRestored) {
            LOG(LL_ERR, "Failed to reinstall Enhanced CleanupReplayPlayback manual hook");
        }

        return originalCallSucceeded && hookRestored;
    }

    bool callEnhancedKillPlaybackOrBakeOriginal(bool userCancelled) {
        const uint64_t targetAddress =
            reinterpret_cast<uint64_t>(pEnhancedKillPlaybackOrBakeOriginal);
        if (targetAddress == 0) {
            LOG(LL_ERR, "Enhanced KillPlaybackOrBake original target is null");
            return false;
        }

        constexpr size_t patchSize = sizeof(g_enhancedKillPlaybackOrBakeOriginalBytes);
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(targetAddress), patchSize,
                            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG(LL_ERR, "Failed to restore Enhanced KillPlaybackOrBake bytes. error=",
                GetLastError());
            return false;
        }

        std::memcpy(reinterpret_cast<void*>(targetAddress),
                    g_enhancedKillPlaybackOrBakeOriginalBytes, patchSize);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(targetAddress), patchSize);
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(targetAddress), patchSize, oldProtect, &unused);

        bool originalCallSucceeded = true;
        try {
            reinterpret_cast<EnhancedKillPlaybackOrBakeFunc>(targetAddress)(userCancelled);
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "Enhanced KillPlaybackOrBake original threw: ", ex.what());
            originalCallSucceeded = false;
        } catch (...) {
            LOG(LL_ERR, "Enhanced KillPlaybackOrBake original threw an unknown exception");
            originalCallSucceeded = false;
        }

        const bool hookRestored = installAbsoluteJumpHook(
            targetAddress,
            reinterpret_cast<uintptr_t>(GameHooks::EnhancedKillPlaybackOrBake::Implementation));
        if (!hookRestored) {
            LOG(LL_ERR, "Failed to reinstall Enhanced KillPlaybackOrBake manual hook");
        }

        return originalCallSucceeded && hookRestored;
    }

    bool prepareEnhancedKillPass1ExitPatch(uint64_t functionAddress) {
        if (functionAddress == 0) return false;

        // The first destructive transition in Enhanced 1.0.1158.13 stores -1 in
        // the recording-instance index. Everything before it includes the call
        // that causes Media Foundation to finalize the current pass. Resolve the
        // boundary from the instruction bytes instead of relying on a bare RVA.
        const auto* bytes = reinterpret_cast<const uint8_t*>(functionAddress);
        for (size_t offset = 0; offset + 12 <= 0x180; ++offset) {
            if (bytes[offset + 0] != 0xC7 || bytes[offset + 1] != 0x05 ||
                bytes[offset + 6] != 0xFF || bytes[offset + 7] != 0xFF ||
                bytes[offset + 8] != 0xFF || bytes[offset + 9] != 0xFF ||
                bytes[offset + 10] != 0xC7 || bytes[offset + 11] != 0x05) {
                continue;
            }

            g_enhancedKillPass1ExitPatchAddress = functionAddress + offset;
            break;
        }

        if (g_enhancedKillPass1ExitPatchAddress == 0) {
            LOG(LL_ERR, "Enhanced KillPlaybackOrBake pass-one exit boundary was not found");
            return false;
        }

        // This stub is entered after the original prologue (push rsi; push rbx;
        // sub rsp, 28h), so it performs the matching epilogue and returns to the
        // original caller without executing the destructive state stores.
        constexpr std::array<uint8_t, 7> exitStub = {
            0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
            0x5B,                   // pop rbx
            0x5E,                   // pop rsi
            0xC3                    // ret
        };
        g_enhancedKillPass1ExitStub = VirtualAlloc(
            nullptr, exitStub.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_enhancedKillPass1ExitStub == nullptr) {
            LOG(LL_ERR, "Failed to allocate Enhanced pass-one KillPlaybackOrBake exit stub. error=",
                GetLastError());
            g_enhancedKillPass1ExitPatchAddress = 0;
            return false;
        }

        std::memcpy(g_enhancedKillPass1ExitStub, exitStub.data(), exitStub.size());
        FlushInstructionCache(GetCurrentProcess(), g_enhancedKillPass1ExitStub, exitStub.size());
        LOG(LL_NFO, "Enhanced KillPlaybackOrBake pass-one exit boundary resolved. patch=",
            Logger::hex(g_enhancedKillPass1ExitPatchAddress, 16), " stub=",
            g_enhancedKillPass1ExitStub);
        return true;
    }

    bool callEnhancedKillPlaybackOrBakeThroughFinalize() {
        if (g_enhancedKillPass1ExitPatchAddress == 0 ||
            g_enhancedKillPass1ExitStub == nullptr) {
            LOG(LL_ERR, "Enhanced pass-one KillPlaybackOrBake exit patch is unavailable");
            return false;
        }

        constexpr size_t patchSize = 12;
        std::array<uint8_t, patchSize> originalInternalBytes{};
        std::memcpy(originalInternalBytes.data(),
                    reinterpret_cast<const void*>(g_enhancedKillPass1ExitPatchAddress),
                    patchSize);

        if (originalInternalBytes[0] != 0xC7 || originalInternalBytes[1] != 0x05 ||
            originalInternalBytes[6] != 0xFF || originalInternalBytes[7] != 0xFF ||
            originalInternalBytes[8] != 0xFF || originalInternalBytes[9] != 0xFF ||
            originalInternalBytes[10] != 0xC7 || originalInternalBytes[11] != 0x05) {
            LOG(LL_ERR, "Enhanced pass-one KillPlaybackOrBake exit bytes changed; refusing partial execution");
            return false;
        }

        if (!installAbsoluteJumpHook(
                g_enhancedKillPass1ExitPatchAddress,
                reinterpret_cast<uintptr_t>(g_enhancedKillPass1ExitStub))) {
            LOG(LL_ERR, "Failed to install temporary Enhanced pass-one exit patch");
            return false;
        }

        LOG(LL_NFO, "Executing Enhanced KillPlaybackOrBake through Finalize trigger, before destructive stores");
        const bool callSucceeded = callEnhancedKillPlaybackOrBakeOriginal(false);

        DWORD oldProtect = 0;
        bool restoreSucceeded = VirtualProtect(
            reinterpret_cast<void*>(g_enhancedKillPass1ExitPatchAddress), patchSize,
            PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE;
        if (restoreSucceeded) {
            std::memcpy(reinterpret_cast<void*>(g_enhancedKillPass1ExitPatchAddress),
                        originalInternalBytes.data(), patchSize);
            FlushInstructionCache(GetCurrentProcess(),
                                  reinterpret_cast<void*>(g_enhancedKillPass1ExitPatchAddress),
                                  patchSize);
            DWORD unused = 0;
            VirtualProtect(reinterpret_cast<void*>(g_enhancedKillPass1ExitPatchAddress),
                           patchSize, oldProtect, &unused);
        } else {
            LOG(LL_ERR, "Failed to restore Enhanced pass-one exit bytes. error=", GetLastError());
        }

        return callSucceeded && restoreSucceeded;
    }

    uint64_t resolveIsPendingBakeStartFunction(uint64_t patternMatchAddress) {
        if (patternMatchAddress == 0) {
            return 0;
        }

        const auto* base = reinterpret_cast<const uint8_t*>(patternMatchAddress);

        for (size_t i = 0; i < 64; ++i) {
            const uint8_t* p = base + i;
            if (p[0] == 0x83 && p[1] == 0x3D && p[6] == 0x04 && p[7] == 0x0F && p[8] == 0x94 && p[9] == 0xC0 && p[10] == 0xC3) {
                return reinterpret_cast<uint64_t>(p);
            }
        }

        return patternMatchAddress;
    }

    uint32_t* resolveIsPendingBakeStartStatePointer(uint64_t functionAddress) {
        if (functionAddress == 0) {
            return nullptr;
        }

        const auto* p = reinterpret_cast<const uint8_t*>(functionAddress);
        if (!(p[0] == 0x83 && p[1] == 0x3D)) {
            return nullptr;
        }

        int32_t disp = 0;
        std::memcpy(&disp, p + 2, sizeof(disp));
        const uint64_t ripAfterCmp = functionAddress + 7;
        return reinterpret_cast<uint32_t*>(ripAfterCmp + disp);
    }

    // Manual hook CleanupReplayPlaybackInternal
    bool installManualHookWithTrampoline(uint64_t targetAddress, uintptr_t hookAddress, void** outTrampoline) {
        if (targetAddress == 0 || hookAddress == 0) {
            return false;
        }

        constexpr size_t patchSize = 12; // Size
        constexpr size_t trampolineSize = patchSize + 12; // Original bytes + jump back

        // Allocate executable memory for trampoline
        void* trampoline = VirtualAlloc(nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!trampoline) {
            LOG(LL_ERR, "Failed to allocate trampoline memory");
            return false;
        }

        // Copy original bytes to trampoline
        std::memcpy(trampoline, reinterpret_cast<void*>(targetAddress), patchSize);

        // Add jump back to (original + patchSize) at end of trampoline
        uint8_t* trampolineBytes = static_cast<uint8_t*>(trampoline);
        uint64_t returnAddress = targetAddress + patchSize;
        
        trampolineBytes[patchSize + 0] = 0x48; // mov rax, imm64
        trampolineBytes[patchSize + 1] = 0xB8;
        std::memcpy(&trampolineBytes[patchSize + 2], &returnAddress, sizeof(returnAddress));
        trampolineBytes[patchSize + 10] = 0xFF; // jmp rax
        trampolineBytes[patchSize + 11] = 0xE0;

        FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineSize);

        if (!installAbsoluteJumpHook(targetAddress, hookAddress)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        *outTrampoline = trampoline;
        LOG(LL_NFO, "Manual hook with trampoline installed at", Logger::hex(targetAddress, 16), 
            "-> hook:", Logger::hex(hookAddress, 16), "trampoline:", trampoline);
        return true;
    }

    void CleanupReplayPlaybackInternal_Hook() {
        PRE();
        LOG(LL_NFO, "CleanupReplayPlaybackInternal called");
        
        if (dualPassContext && dualPassContext->state == DualPassState::PASS1_COMPLETE) {
            if (g_pass2TriggerIssued.exchange(true)) {
                LOG(LL_DBG, "Pass 2 trigger already issued; ignoring duplicate CleanupReplayPlaybackInternal handoff");
                POST();
                return;
            }

            LOG(LL_NFO, "Intercepting CleanupReplayPlaybackInternal after Pass 1");
            try {
                uint64_t cleanupAddr = reinterpret_cast<uint64_t>(pCleanupReplayPlaybackOriginal);

                LOG(LL_DBG, "Temporarily restoring original bytes at 0x", std::hex, cleanupAddr);
                
                // Restore original bytes
                DWORD oldProtect;
                if (!VirtualProtect(reinterpret_cast<void*>(cleanupAddr), 12, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    LOG(LL_ERR, "VirtualProtect failed (restore): ", GetLastError());
                    dualPassContext->state = DualPassState::IDLE;
                    dualPassContext->audio_buffer.clear();
                    g_pass2TriggerIssued = false;
                    POST();
                    return;
                }
                
                memcpy(reinterpret_cast<void*>(cleanupAddr), g_cleanupOriginalBytes, 12);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(cleanupAddr), 12);
                
                LOG(LL_DBG, "Calling original CleanupReplayPlaybackInternal...");
                reinterpret_cast<CleanupReplayPlaybackInternalFunc>(cleanupAddr)();
                LOG(LL_NFO, "Original cleanup completed - playback controller reset");
                
                // Re-install the hook
                LOG(LL_DBG, "Re-installing hook...");
                installAbsoluteJumpHook(cleanupAddr, 
                    reinterpret_cast<uintptr_t>(&CleanupReplayPlaybackInternal_Hook));
                
                VirtualProtect(reinterpret_cast<void*>(cleanupAddr), 12, oldProtect, &oldProtect);
            }
            catch (const std::exception& e) {
                LOG(LL_ERR, "Exception during unhook/rehook: ", e.what());
                dualPassContext->state = DualPassState::IDLE;
                dualPassContext->audio_buffer.clear();
                g_pass2TriggerIssued = false;
                POST();
                return;
            }
            catch (...) {
                LOG(LL_ERR, "Unknown exception during unhook/rehook");
                dualPassContext->state = DualPassState::IDLE;
                dualPassContext->audio_buffer.clear();
                g_pass2TriggerIssued = false;
                POST();
                return;
            }

            schedulePass2StartupOnMainThread("CleanupReplayPlaybackInternal(pass1-complete)");
            LOG(LL_NFO, "Pass 2 scheduled - will execute on GTA main thread");
            POST();
            return;
            
        } else {
            // Normal cleanup
            LOG(LL_DBG, "Normal CleanupReplayPlaybackInternal execution");
            if (dualPassContext) {
                LOG(LL_DBG, "  Dual-pass state: ", static_cast<int>(dualPassContext->state));
                LOG(LL_DBG, "  Calling original cleanup to free resources properly");
            }
            
            // Temporarily unhook to call original
            try {
                uint64_t cleanupAddr = reinterpret_cast<uint64_t>(pCleanupReplayPlaybackOriginal);
                
                DWORD oldProtect;
                if (!VirtualProtect(reinterpret_cast<void*>(cleanupAddr), 12, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    LOG(LL_ERR, "VirtualProtect failed (normal cleanup): ", GetLastError());
                    POST();
                    return;
                }
                
                // Restore original bytes
                memcpy(reinterpret_cast<void*>(cleanupAddr), g_cleanupOriginalBytes, 12);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(cleanupAddr), 12);
                
                // Call original
                reinterpret_cast<CleanupReplayPlaybackInternalFunc>(cleanupAddr)();
                
                // Re-install hook
                installAbsoluteJumpHook(cleanupAddr, 
                    reinterpret_cast<uintptr_t>(&CleanupReplayPlaybackInternal_Hook));
                
                VirtualProtect(reinterpret_cast<void*>(cleanupAddr), 12, oldProtect, &oldProtect);
                
                LOG(LL_DBG, "Original cleanup completed and hook reinstalled");
            }
            catch (const std::exception& e) {
                LOG(LL_ERR, "Exception during normal cleanup: ", e.what());
            }
            catch (...) {
                LOG(LL_ERR, "Unknown exception during normal cleanup");
            }
            
            // Reset dual-pass after Pass 2 completes (if in dual-pass mode)
            if (dualPassContext && dualPassContext->state == DualPassState::PASS2_COMPLETE) {
                LOG(LL_NFO, "PASS 2 CLEANUP COMPLETE");
                LOG(LL_NFO, "Resetting dual-pass context to IDLE");
                LOG(LL_DBG, "  KillPlaybackOrBake should have been called and executed normally");
                LOG(LL_DBG, "  Recording instance should be freed (ms_recordingInstanceIndex = INDEX_NONE)");
                LOG(LL_DBG, "  Ready for next export");
                
                dualPassContext->state = DualPassState::IDLE;
                dualPassContext->audio_buffer.clear();
                dualPassContext->audio_buffer.shrink_to_fit();
                dualPassContext->saved_video_editor_interface = nullptr;
                dualPassContext->saved_montage_ptr = nullptr;
                
                LOG(LL_NFO, "Dual-pass context reset complete");
            }
        }
        
        LOG(LL_NFO, "CleanupReplayPlaybackInternal exit");
        POST();
    }

    bool IsPendingBakeStart_Hook() {
        PRE();
        
        bool originalResult = false;
        if (g_isPendingBakeStartStatePtr) {
            originalResult = (*g_isPendingBakeStartStatePtr == 4);
        }
        
        LOG(LL_TRC, "IsPendingBakeStart called, original result: ", originalResult);
        
        if (g_isEnhancedEdition && dualPassContext &&
            dualPassContext->state == DualPassState::PASS1_COMPLETE &&
            !g_pass2TriggerIssued.load() &&
            pCleanupReplayPlaybackOriginal != nullptr &&
            std::this_thread::get_id() == mainThreadId &&
            !g_enhancedIsPendingHandoffInProgress) {
            g_enhancedIsPendingHandoffInProgress = true;
            LOG(LL_NFO, "IsPendingBakeStart: triggering Enhanced pass-one cleanup on the main thread");
            try {
                reinterpret_cast<CleanupReplayPlaybackInternalFunc>(
                    pCleanupReplayPlaybackOriginal)();
            } catch (const std::exception& ex) {
                LOG(LL_ERR, "IsPendingBakeStart cleanup handoff exception: ", ex.what());
                g_pass2TriggerIssued = false;
            } catch (...) {
                LOG(LL_ERR, "IsPendingBakeStart cleanup handoff unknown exception");
                g_pass2TriggerIssued = false;
            }
            g_enhancedIsPendingHandoffInProgress = false;
        }

        if (g_isEnhancedEdition && dualPassContext &&
            dualPassContext->state == DualPassState::PASS2_PENDING &&
            std::this_thread::get_id() == mainThreadId) {
            // Returning TRUE keeps Enhanced's loading gate active and pauses
            // ScriptMain. Release it temporarily so ScriptMain can count the
            // normal frame delay and start Pass 2 from a settled game frame.
            LOG(LL_NFO, "IsPendingBakeStart: releasing Enhanced loading gate for delayed pass-two startup");
            POST();
            return false;
        }

        if (g_isEnhancedEdition && dualPassContext &&
            dualPassContext->state == DualPassState::PASS2_COMPLETE &&
            pCleanupReplayPlaybackOriginal != nullptr &&
            std::this_thread::get_id() == mainThreadId &&
            !g_enhancedIsPendingHandoffInProgress) {
            // Enhanced may leave the pending-bake screen active after the
            // second sink finalizes. Its script fiber is still paused here, so
            // perform the normal replay cleanup from the same main-thread poll
            // that owns the UI state.
            g_enhancedIsPendingHandoffInProgress = true;
            LOG(LL_NFO, "IsPendingBakeStart: triggering Enhanced pass-two final cleanup on the main thread");
            try {
                reinterpret_cast<CleanupReplayPlaybackInternalFunc>(
                    pCleanupReplayPlaybackOriginal)();
            } catch (const std::exception& ex) {
                LOG(LL_ERR, "IsPendingBakeStart final cleanup exception: ", ex.what());
            } catch (...) {
                LOG(LL_ERR, "IsPendingBakeStart final cleanup unknown exception");
            }
            g_enhancedIsPendingHandoffInProgress = false;

            if (dualPassContext->state == DualPassState::IDLE) {
                LOG(LL_NFO, "IsPendingBakeStart: Enhanced dual-pass UI cleanup completed");
                POST();
                return false;
            }
        }

        if (dualPassContext) {
            if (dualPassContext->state == DualPassState::PASS1_COMPLETE ||
                (!g_isEnhancedEdition && dualPassContext->state == DualPassState::PASS2_PENDING)) {
                LOG(LL_DBG, "IsPendingBakeStart: Overriding to TRUE for dual-pass transition");
                LOG(LL_DBG, "  Dual-pass state: ", static_cast<int>(dualPassContext->state));
                LOG(LL_DBG, "  Keeping loading screen visible until Pass 2 starts");
                POST();
                return true;
            }
        }
        
        // Normal behavior - return GTA's state
        if (dualPassContext && originalResult) {
            LOG(LL_TRC, "IsPendingBakeStart: Dual-pass state: ", static_cast<int>(dualPassContext->state));
        }
        
        POST();
        return originalResult;
    }

    void KillPlaybackOrBake_Hook(void* thisPtr, bool userCancelled) {
        PRE();
        LOG(LL_NFO, "KillPlaybackOrBake_Hook called");
        LOG(LL_DBG, "  thisPtr: ", thisPtr);
        LOG(LL_DBG, "  userCancelled: ", userCancelled);
        LOG(LL_DBG, "  dualPassContext exists: ", (dualPassContext != nullptr));
        if (dualPassContext) {
            LOG(LL_DBG, "  Current dual-pass state: ", static_cast<int>(dualPassContext->state),
                " (",
                (dualPassContext->state == DualPassState::IDLE ? "IDLE" :
                 dualPassContext->state == DualPassState::PASS1_RUNNING ? "PASS1_RUNNING" :
                 dualPassContext->state == DualPassState::PASS1_COMPLETE ? "PASS1_COMPLETE" :
                 dualPassContext->state == DualPassState::PASS2_PENDING ? "PASS2_PENDING" :
                 dualPassContext->state == DualPassState::PASS2_RUNNING ? "PASS2_RUNNING" :
                 dualPassContext->state == DualPassState::PASS2_COMPLETE ? "PASS2_COMPLETE" : "UNKNOWN"),
                ")");
        }
        
        if (dualPassContext && dualPassContext->state == DualPassState::PASS1_COMPLETE) {
            LOG(LL_NFO, "Suppressing KillPlaybackOrBake during PASS1_COMPLETE to preserve pass handoff");
            tryForceWantDelayedCloseFalse("KillPlaybackOrBake_Hook(pass1-complete)");
            POST();
            return;
        }
        
        LOG(LL_NFO, "KillPlaybackOrBake (normal cleanup)");
        if (dualPassContext) {
            LOG(LL_DBG, "  Current state: ", static_cast<int>(dualPassContext->state),
                " (",
                (dualPassContext->state == DualPassState::IDLE ? "IDLE" :
                 dualPassContext->state == DualPassState::PASS2_COMPLETE ? "PASS2_COMPLETE" :
                 dualPassContext->state == DualPassState::PASS2_RUNNING ? "PASS2_RUNNING" : "OTHER"),
                ")");
        } else {
            LOG(LL_DBG, "  No dual-pass context (normal single-pass mode)");
        }
        
        try {
            uint64_t killAddr = reinterpret_cast<uint64_t>(pKillPlaybackOrBakeOriginal);
            
            LOG(LL_DBG, "Temporarily restoring original bytes at 0x", std::hex, killAddr);
            
            DWORD oldProtect;
            if (!VirtualProtect(reinterpret_cast<void*>(killAddr), 12, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG(LL_ERR, "VirtualProtect failed (restore): ", GetLastError());
                POST();
                return;
            }
            
            memcpy(reinterpret_cast<void*>(killAddr), g_killPlaybackOrBakeOriginalBytes, 12);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(killAddr), 12);
            
            LOG(LL_DBG, "Calling original KillPlaybackOrBake function...");
            LOG(LL_DBG, "  Function address: 0x", std::hex, killAddr, std::dec);

            reinterpret_cast<KillPlaybackOrBakeFunc>(killAddr)(thisPtr, userCancelled);
            
            LOG(LL_NFO, "Original KillPlaybackOrBake executed successfully");
            LOG(LL_DBG, "  ms_recordingInstanceIndex should now be INDEX_NONE (-1)");
            LOG(LL_DBG, "  Recording slot should be freed for next export");
            
            // Re-install the hook
            LOG(LL_DBG, "Re-installing hook...");
            installAbsoluteJumpHook(killAddr, 
                reinterpret_cast<uintptr_t>(&KillPlaybackOrBake_Hook));
            
            VirtualProtect(reinterpret_cast<void*>(killAddr), 12, oldProtect, &oldProtect);
            
            LOG(LL_DBG, "Hook reinstalled successfully");
        }
        catch (const std::exception& e) {
            LOG(LL_ERR, "Exception during unhook/rehook: ", e.what());
            LOG(LL_ERR, "  This may leave the hook in an inconsistent state!");
        }
        catch (...) {
            LOG(LL_ERR, "Unknown exception during unhook/rehook");
            LOG(LL_ERR, "  This may leave the hook in an inconsistent state!");
        }
        
        LOG(LL_NFO, "KillPlaybackOrBake_Hook exit");
        POST();
    }

    void SetUserConfirmationScreen_Hook(const char* pTextLabel,
                                        const char* pTitle,
                                        uint32_t type,
                                        bool allowSpinner,
                                        const char* literalString,
                                        const char* literalString2) {
        PRE();

        const std::string safeLabel = SafeAnsiString(pTextLabel);
        const std::string safeTitle = SafeAnsiString(pTitle);

        LOG(LL_DBG, "SetUserConfirmationScreen_Hook called. label=", safeLabel,
            " title=", safeTitle,
            " type=", type,
            " allowSpinner=", allowSpinner,
            " dualPass=", dualPassContext ? dualPassStateName(dualPassContext->state) : "NO_CONTEXT");

        if (asyncFinalizeInProgress.load() && isExportCompletionLabel(pTextLabel)) {
            LOG(LL_NFO, "Suppressing export-complete popup while async FFmpeg finalization is running.");
            tryForceWantDelayedCloseFalse("SetUserConfirmationScreen_Hook(async-finalize)");
            POST();
            return;
        }

        if (asyncFinalizeCompleted.load() &&
            (isExportCompletionLabel(pTextLabel) || std::strcmp(safeLabel.c_str(), "VE_BAKE_ERROR") == 0)) {
            LOG(LL_NFO, "Clearing async finalization state after terminal export dialog. label=", safeLabel);
            clearAsyncFinalizeState();
        }

        if (dualPassContext &&
            isPass1TransitionSuppressLabel(pTextLabel) &&
            (dualPassContext->state == DualPassState::PASS1_RUNNING ||
             dualPassContext->state == DualPassState::PASS1_COMPLETE)) {

            if (dualPassContext->state == DualPassState::PASS1_RUNNING) {
                dualPassContext->state = DualPassState::PASS1_COMPLETE;
                LOG(LL_NFO, "Pass 1 completion popup suppressed. State forced to PASS1_COMPLETE");
            } else {
                LOG(LL_NFO, "Pass 1 completion popup suppressed. State already PASS1_COMPLETE");
            }

            if (std::strcmp(safeLabel.c_str(), "VE_BAKE_ERROR") == 0) {
                LOG(LL_NFO, "Suppressing VE_BAKE_ERROR during PASS1 handoff (expected transient in dual-pass)");
            }

            g_pass2TriggerIssued = false;
            tryForceWantDelayedCloseFalse("SetUserConfirmationScreen_Hook(pass1-success)");

            POST();
            return;
        }

        try {
            uint64_t targetAddr = reinterpret_cast<uint64_t>(pSetUserConfirmationScreenOriginal);
            DWORD oldProtect = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(targetAddr), 12, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG(LL_ERR, "SetUserConfirmationScreen_Hook: VirtualProtect restore failed: ", GetLastError());
                POST();
                return;
            }

            std::memcpy(reinterpret_cast<void*>(targetAddr), g_setUserConfirmationScreenOriginalBytes, 12);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(targetAddr), 12);

            reinterpret_cast<SetUserConfirmationScreenFunc>(targetAddr)(
                pTextLabel,
                pTitle,
                type,
                allowSpinner,
                literalString,
                literalString2);

            installAbsoluteJumpHook(targetAddr, reinterpret_cast<uintptr_t>(&SetUserConfirmationScreen_Hook));
            VirtualProtect(reinterpret_cast<void*>(targetAddr), 12, oldProtect, &oldProtect);
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "SetUserConfirmationScreen_Hook exception: ", ex.what());
        } catch (...) {
            LOG(LL_ERR, "SetUserConfirmationScreen_Hook unknown exception");
        }

        POST();
    }

    void touchD3D11Import() {
        static auto d3d11Import = &D3D11CreateDeviceAndSwapChain;
        (void)d3d11Import;
    }

    bool installD3D11Hook(HMODULE module) {
        if (module == nullptr) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastD3D11HookAttempt < kD3D11HookRetryDelay) {
            LOG(LL_TRC, "Skipping D3D11 hook attempt (debounce window)");
            return isD3D11HookInstalled;
        }
        lastD3D11HookAttempt = now;

        LOG(LL_DBG, "installD3D11Hook invoked. module:", module);
        if (isInstallingD3D11Hook) {
            LOG(LL_TRC, "D3D11 hook installation already in progress; skipping nested request");
            return true;
        }

        ScopedFlagGuard installGuard(isInstallingD3D11Hook);
        std::scoped_lock hookLock(d3d11HookMutex);
        if (isD3D11HookInstalled) {
            LOG(LL_TRC, "D3D11 hook already installed");
            return true;
        }

        try {
            LOG(LL_DBG, "Attempting to hook D3D11CreateDeviceAndSwapChain export");
            const auto proc = GetProcAddress(module, "D3D11CreateDeviceAndSwapChain");
            if (proc == nullptr) {
                LOG(LL_ERR, "GetProcAddress failed for D3D11CreateDeviceAndSwapChain");
                return false;
            }

            const auto address = reinterpret_cast<uint64_t>(proc);
            const HRESULT hr = ever::hooking::hookX64Function(address, reinterpret_cast<void*>(ExportHooks::D3D11CreateDeviceAndSwapChain::Implementation),
                                            &ExportHooks::D3D11CreateDeviceAndSwapChain::OriginalFunc,
                                            ExportHooks::D3D11CreateDeviceAndSwapChain::Detour,
                                            PLH::x64Detour::detour_scheme_t::INPLACE);
            if (FAILED(hr)) {
                LOG(LL_ERR, "hookX64Function failed for D3D11CreateDeviceAndSwapChain: ", hr);
                return false;
            }

            isD3D11HookInstalled = true;
            LOG(LL_DBG, "D3D11CreateDeviceAndSwapChain hook installed via detour");
        } catch (std::exception& ex) {
            LOG(LL_ERR, "Failed to hook D3D11CreateDeviceAndSwapChain: ", ex.what());
            if (!hasLoggedD3D11HookFailure.exchange(true)) {
                LOG(LL_ERR, "D3D11 hook failed once; will keep retrying lazily");
            }
        } catch (...) {
            LOG(LL_ERR, "Failed to hook D3D11CreateDeviceAndSwapChain");
            if (!hasLoggedD3D11HookFailure.exchange(true)) {
                LOG(LL_ERR, "D3D11 hook failed with unknown exception");
            }
        }

        return isD3D11HookInstalled;
    }

    void schedulePass2StartupOnMainThread(const char* reason) {
        if (!dualPassContext) {
            LOG(LL_ERR, "schedulePass2StartupOnMainThread: dualPassContext is NULL. reason=", reason);
            g_pass2TriggerIssued = false;
            g_pass2StartupPending = false;
            return;
        }

        if (dualPassContext->state != DualPassState::PASS1_COMPLETE &&
            dualPassContext->state != DualPassState::PASS2_PENDING) {
            LOG(LL_WRN, "schedulePass2StartupOnMainThread: unexpected state ",
                dualPassStateName(dualPassContext->state), " reason=", reason);
            return;
        }

        dualPassContext->state = DualPassState::PASS2_PENDING;
        // Enhanced releases its pending-bake gate below so ScriptMain can
        // advance this delay per rendered frame, just like Legacy.
        const int startupDelayTicks = kPass2StartupDelayFrames;
        g_pass2StartupDelayFrames = startupDelayTicks;
        g_pass2StartupPending = true;
        g_pass2TriggerIssued = true;

        LOG(LL_NFO, "Scheduling Pass 2 startup on main thread after ",
            Logger::hex(static_cast<uint64_t>(startupDelayTicks), 2),
            " frames. reason=", reason);
    }

    bool executePass2StartupOnMainThread() {
        if (!g_pass2StartupPending.load()) {
            return false;
        }

        if (std::this_thread::get_id() != mainThreadId) {
            return false;
        }

        const int remainingDelay = g_pass2StartupDelayFrames.load();
        if (remainingDelay > 0) {
            g_pass2StartupDelayFrames = remainingDelay - 1;
            return false;
        }

        g_pass2StartupPending = false;

        LOG(LL_NFO, "=== Pass 2 Startup (Main Thread) ===");

        if (!dualPassContext) {
            LOG(LL_ERR, "Pass 2 startup: dualPassContext is NULL");
            g_pass2TriggerIssued = false;
            return false;
        }

        if (dualPassContext->state != DualPassState::PASS2_PENDING &&
            dualPassContext->state != DualPassState::PASS1_COMPLETE) {
            LOG(LL_WRN, "Pass 2 startup skipped due to state=", dualPassStateName(dualPassContext->state));
            g_pass2TriggerIssued = false;
            return false;
        }

        const bool startFunctionAvailable = g_isEnhancedEdition
            ? GameHooks::EnhancedStartBakeProject::OriginalFunc != nullptr
            : GameHooks::StartBakeProject::OriginalFunc != nullptr;
        if (!startFunctionAvailable) {
            LOG(LL_ERR, "Pass 2 startup: edition-specific StartBakeProject::OriginalFunc is NULL");
            dualPassContext->state = DualPassState::IDLE;
            dualPassContext->audio_buffer.clear();
            g_pass2TriggerIssued = false;
            return false;
        }

        const bool savedArgumentsValid = g_isEnhancedEdition
            ? dualPassContext->saved_video_editor_interface != nullptr
            : dualPassContext->saved_video_editor_interface != nullptr &&
              dualPassContext->saved_montage_ptr != nullptr;
        if (!savedArgumentsValid) {
            LOG(LL_ERR, "Pass 2 startup: saved pointers are invalid. interface=",
                reinterpret_cast<void*>(dualPassContext->saved_video_editor_interface),
                " montage=", reinterpret_cast<void*>(dualPassContext->saved_montage_ptr));
            dualPassContext->state = DualPassState::IDLE;
            dualPassContext->audio_buffer.clear();
            g_pass2TriggerIssued = false;
            return false;
        }

        LOG(LL_NFO, "Starting Pass 2 (Video Export)");
        dualPassContext->state = DualPassState::PASS2_PENDING;
        Config::Manager::fps = dualPassContext->original_fps;
        Config::Manager::motion_blur_samples = dualPassContext->original_motion_blur_samples;

        LOG(LL_DBG, "  Restoring FPS: ", Config::Manager::fps.first, "/", Config::Manager::fps.second);
        LOG(LL_DBG, "  Motion blur: ", Config::Manager::motion_blur_samples);
        LOG(LL_DBG, "  Video editor interface ptr: ", reinterpret_cast<void*>(dualPassContext->saved_video_editor_interface));
        LOG(LL_DBG, "  Montage ptr: ", reinterpret_cast<void*>(dualPassContext->saved_montage_ptr));
        LOG(LL_DBG, "  Audio buffer size: ", dualPassContext->audio_buffer.size(), " bytes");

        dualPassContext->state = DualPassState::PASS2_RUNNING;

        bool result = false;
        try {
            if (g_isEnhancedEdition) {
                const char* exportName = dualPassContext->enhanced_export_name_present
                    ? dualPassContext->enhanced_export_name.c_str()
                    : nullptr;
                result = GameHooks::EnhancedStartBakeProject::OriginalFunc(
                    dualPassContext->saved_video_editor_interface,
                    exportName);
            } else {
                result = GameHooks::StartBakeProject::OriginalFunc(
                    dualPassContext->saved_video_editor_interface,
                    dualPassContext->saved_montage_ptr);
            }
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "Pass 2 startup exception: ", ex.what());
        } catch (...) {
            LOG(LL_ERR, "Pass 2 startup unknown exception");
        }

        LOG(LL_NFO, "Pass 2 StartBakeProject returned: ", result);
        if (!result) {
            LOG(LL_ERR, "Pass 2 failed to start; resetting dual-pass context");
            dualPassContext->state = DualPassState::IDLE;
            dualPassContext->audio_buffer.clear();
            dualPassContext->audio_buffer.shrink_to_fit();
            g_pass2TriggerIssued = false;
            return false;
        }

        return true;
    }


    std::string getD3D12ResourceDebugName(reshade::api::device* device, reshade::api::resource resource) {
        if (device == nullptr || resource.handle == 0 ||
            device->get_api() != reshade::api::device_api::d3d12) {
            return {};
        }

        auto* nativeResource = reinterpret_cast<ID3D12Resource*>(resource.handle);
        UINT sizeBytes = 0;
        if (SUCCEEDED(nativeResource->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sizeBytes, nullptr)) &&
            sizeBytes >= sizeof(wchar_t)) {
            std::vector<wchar_t> wideName(sizeBytes / sizeof(wchar_t) + 1, L'\0');
            if (SUCCEEDED(nativeResource->GetPrivateData(
                    WKPDID_D3DDebugObjectNameW, &sizeBytes, wideName.data()))) {
                size_t length = sizeBytes / sizeof(wchar_t);
                while (length > 0 && wideName[length - 1] == L'\0') --length;
                return utf8_encode(std::wstring(wideName.data(), length));
            }
        }

        sizeBytes = 0;
        if (SUCCEEDED(nativeResource->GetPrivateData(WKPDID_D3DDebugObjectName, &sizeBytes, nullptr)) &&
            sizeBytes > 0) {
            std::string ansiName(sizeBytes, '\0');
            if (SUCCEEDED(nativeResource->GetPrivateData(
                    WKPDID_D3DDebugObjectName, &sizeBytes, ansiName.data()))) {
                while (!ansiName.empty() && ansiName.back() == '\0') ansiName.pop_back();
                return ansiName;
            }
        }

        return {};
    }

    void tryTrackEnhancedVideoResource(reshade::api::device* device, reshade::api::resource resource,
                                       reshade::api::resource_usage state) {
        if (!g_isEnhancedEdition || device == nullptr || resource.handle == 0 ||
            device->get_api() != reshade::api::device_api::d3d12) {
            return;
        }

        const std::string name = getD3D12ResourceDebugName(device, resource);
        if (name.rfind("VideoEncode", 0) != 0) return;

        const reshade::api::resource_desc desc = device->get_resource_desc(resource);
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        if (enhancedVideoEncodeResource.handle != resource.handle) {
            enhancedVideoEncodeResource = resource;
            enhancedVideoEncodeState = state;
            enhancedVideoResourceTracked.store(true, std::memory_order_release);
            LOG(LL_NFO, "Enhanced VideoEncode resource discovered via D3D12 debug name. name=", name,
                " handle=", Logger::hex(resource.handle, 16), " resolution=",
                desc.texture.width, "x", desc.texture.height,
                " format=", static_cast<uint32_t>(desc.texture.format));
        } else {
            enhancedVideoEncodeState = state;
        }
    }

    void resetEnhancedCaptureTracking() {
        enhancedPendingVideoFrames.store(0, std::memory_order_release);
        enhancedExpectedWidth.store(0, std::memory_order_release);
        enhancedExpectedHeight.store(0, std::memory_order_release);
        enhancedVideoResourceTracked.store(false, std::memory_order_release);
        enhancedCaptureGeneration.fetch_add(1, std::memory_order_acq_rel);

        {
            std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
            enhancedVideoEncodeResource = {};
            enhancedVideoEncodeState = reshade::api::resource_usage::undefined;
            enhancedCleanCandidateDevice = nullptr;
            enhancedCleanCandidateResource = {};
            enhancedCleanCandidateState = reshade::api::resource_usage::undefined;
            enhancedDepthCandidateDevice = nullptr;
            enhancedDepthCandidateResource = {};
            enhancedDepthCandidateState = reshade::api::resource_usage::undefined;
        }

        {
            std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
            enhancedLoggedRenderTargetHandles.clear();
            enhancedLoggedDepthStencilHandles.clear();
            enhancedLoggedCopySourceHandles.clear();
        }
        hasLoggedEnhancedBackBuffer.store(false, std::memory_order_release);
        hasLoggedEnhancedCleanCapture.store(false, std::memory_order_release);
        enhancedPostEffectsCaptureReady.store(false, std::memory_order_release);
        enhancedPostEffectsBackBufferHandle.store(0, std::memory_order_release);
        hasLoggedEnhancedPreEffectsReplacement.store(false, std::memory_order_release);
        hasLoggedEnhancedMissingDepth.store(false, std::memory_order_release);
        enhancedOpenExrPayloadValidated.store(false, std::memory_order_release);
        enhancedCaptureDepth.clear();
        enhancedMotionBlur = {};
        enhancedGpuMotionBlur.subframeIndex = 0;
        enhancedGpuMotionBlur.accumulatedSamples = 0;
        enhancedGpuMotionBlur.configuredSamples = 0;
        enhancedGpuMotionBlur.configuredStrength = 0.0f;
        enhancedGpuMotionBlur.consumedSubframes = 0;
        enhancedGpuMotionBlur.emittedFrames = 0;
        enhancedGpuMotionBlur.creationFailed = false;
    }

    bool copyEnhancedMappedPixels(const reshade::api::subresource_data& mapped,
                                  reshade::api::format format,
                                  uint32_t width,
                                  uint32_t height,
                                  std::vector<uint8_t>& pixels) {
        const reshade::api::format viewFormat = reshade::api::format_to_default_typed(format, 0);
        const size_t outputRowPitch = static_cast<size_t>(width) * 4;
        pixels.resize(outputRowPitch * static_cast<size_t>(height));

        auto* destination = pixels.data();
        auto* sourcePixels = static_cast<const uint8_t*>(mapped.data);
        for (uint32_t y = 0; y < height; ++y,
             destination += outputRowPitch, sourcePixels += mapped.row_pitch) {
            switch (viewFormat) {
            case reshade::api::format::r8g8b8a8_unorm:
            case reshade::api::format::r8g8b8x8_unorm:
                std::memcpy(destination, sourcePixels, outputRowPitch);
                if (viewFormat == reshade::api::format::r8g8b8x8_unorm)
                    for (size_t x = 3; x < outputRowPitch; x += 4) destination[x] = 0xFF;
                break;
            case reshade::api::format::b8g8r8a8_unorm:
            case reshade::api::format::b8g8r8x8_unorm:
                std::memcpy(destination, sourcePixels, outputRowPitch);
                for (size_t x = 0; x < outputRowPitch; x += 4) {
                    std::swap(destination[x], destination[x + 2]);
                    if (viewFormat == reshade::api::format::b8g8r8x8_unorm) destination[x + 3] = 0xFF;
                }
                break;
            case reshade::api::format::r10g10b10a2_unorm:
            case reshade::api::format::b10g10r10a2_unorm:
                for (uint32_t x = 0; x < width; ++x) {
                    const uint32_t packed = reinterpret_cast<const uint32_t*>(sourcePixels)[x];
                    destination[x * 4 + 0] = static_cast<uint8_t>((packed & 0x3FF) >> 2);
                    destination[x * 4 + 1] = static_cast<uint8_t>(((packed >> 10) & 0x3FF) >> 2);
                    destination[x * 4 + 2] = static_cast<uint8_t>(((packed >> 20) & 0x3FF) >> 2);
                    destination[x * 4 + 3] = static_cast<uint8_t>(((packed >> 30) & 0x3) * 85);
                    if (viewFormat == reshade::api::format::b10g10r10a2_unorm)
                        std::swap(destination[x * 4], destination[x * 4 + 2]);
                }
                break;
            default:
                return false;
            }
        }
        return true;
    }

    reshade::api::format enhancedDepthReadbackPlaneFormat(reshade::api::format sourceFormat) {
        switch (reshade::api::format_to_depth_stencil_typed(sourceFormat)) {
        case reshade::api::format::d16_unorm:
            return reshade::api::format::r16_unorm;
        case reshade::api::format::d24_unorm_s8_uint:
            return reshade::api::format::r24_unorm_x8_uint;
        case reshade::api::format::d32_float:
        case reshade::api::format::d32_float_s8_uint:
            // Plane 0 of D32/S8 is a tightly packed R32_FLOAT plane. A readback
            // footprint using the combined eight-byte depth/stencil format is
            // invalid and causes the entire D3D12 copy batch to read as zeros.
            return reshade::api::format::r32_float;
        default:
            return reshade::api::format::unknown;
        }
    }

    bool copyEnhancedMappedDepth(const reshade::api::subresource_data& mapped,
                                 reshade::api::format format,
                                 uint32_t width,
                                 uint32_t height,
                                 std::vector<float>& depth) {
        if (mapped.data == nullptr || mapped.row_pitch == 0 || width == 0 || height == 0) {
            return false;
        }

        const reshade::api::format typedFormat =
            reshade::api::format_to_default_typed(format, 0);
        depth.resize(static_cast<size_t>(width) * height);

        for (uint32_t y = 0; y < height; ++y) {
            const auto* sourceRow = static_cast<const uint8_t*>(mapped.data) +
                                    static_cast<size_t>(y) * mapped.row_pitch;
            float* destinationRow = depth.data() + static_cast<size_t>(y) * width;

            switch (typedFormat) {
            case reshade::api::format::r16_unorm:
                for (uint32_t x = 0; x < width; ++x) {
                    destinationRow[x] = static_cast<float>(
                        reinterpret_cast<const uint16_t*>(sourceRow)[x]) / 65535.0f;
                }
                break;
            case reshade::api::format::r24_unorm_x8_uint:
                for (uint32_t x = 0; x < width; ++x) {
                    const uint32_t packed = reinterpret_cast<const uint32_t*>(sourceRow)[x];
                    destinationRow[x] = static_cast<float>(packed & 0x00FFFFFFu) / 16777215.0f;
                }
                break;
            case reshade::api::format::r32_float:
                std::memcpy(destinationRow, sourceRow, static_cast<size_t>(width) * sizeof(float));
                break;
            default:
                depth.clear();
                return false;
            }
        }

        return true;
    }

    void destroyEnhancedGpuMotionBlurResourcesLocked() {
        auto* const device = enhancedGpuMotionBlur.device;
        if (device != nullptr) {
            if (enhancedGpuMotionBlur.sourceSrv.handle != 0)
                device->destroy_resource_view(enhancedGpuMotionBlur.sourceSrv);
            if (enhancedGpuMotionBlur.accumulationSrv.handle != 0)
                device->destroy_resource_view(enhancedGpuMotionBlur.accumulationSrv);
            if (enhancedGpuMotionBlur.accumulationRtv.handle != 0)
                device->destroy_resource_view(enhancedGpuMotionBlur.accumulationRtv);
            if (enhancedGpuMotionBlur.finalRtv.handle != 0)
                device->destroy_resource_view(enhancedGpuMotionBlur.finalRtv);
            if (enhancedGpuMotionBlur.accumulatePipeline.handle != 0)
                device->destroy_pipeline(enhancedGpuMotionBlur.accumulatePipeline);
            if (enhancedGpuMotionBlur.dividePipeline.handle != 0)
                device->destroy_pipeline(enhancedGpuMotionBlur.dividePipeline);
            if (enhancedGpuMotionBlur.pipelineLayout.handle != 0)
                device->destroy_pipeline_layout(enhancedGpuMotionBlur.pipelineLayout);
            if (enhancedGpuMotionBlur.sourceCopy.handle != 0)
                device->destroy_resource(enhancedGpuMotionBlur.sourceCopy);
            if (enhancedGpuMotionBlur.accumulation.handle != 0)
                device->destroy_resource(enhancedGpuMotionBlur.accumulation);
            if (enhancedGpuMotionBlur.finalOutput.handle != 0)
                device->destroy_resource(enhancedGpuMotionBlur.finalOutput);
        }
        enhancedGpuMotionBlur = {};
    }

    bool ensureEnhancedGpuMotionBlurResourcesLocked(
        reshade::api::effect_runtime* runtime,
        const reshade::api::resource_desc& sourceDesc) {
        if (runtime == nullptr || runtime->get_device() == nullptr ||
            runtime->get_device()->get_api() != reshade::api::device_api::d3d12 ||
            Config::Manager::export_openexr) {
            return false;
        }

        auto* const device = runtime->get_device();
        const reshade::api::format sourceFormat =
            reshade::api::format_to_default_typed(sourceDesc.texture.format, 0);
        const bool supportedFormat =
            sourceFormat == reshade::api::format::r8g8b8a8_unorm ||
            sourceFormat == reshade::api::format::b8g8r8a8_unorm;
        if (!supportedFormat || sourceDesc.texture.samples != 1) return false;

        if (enhancedGpuMotionBlur.runtime == runtime &&
            enhancedGpuMotionBlur.device == device &&
            enhancedGpuMotionBlur.width == sourceDesc.texture.width &&
            enhancedGpuMotionBlur.height == sourceDesc.texture.height &&
            enhancedGpuMotionBlur.sourceFormat == sourceFormat &&
            enhancedGpuMotionBlur.sourceCopy.handle != 0 &&
            !enhancedGpuMotionBlur.creationFailed) {
            return true;
        }

        destroyEnhancedGpuMotionBlurResourcesLocked();
        enhancedGpuMotionBlur.runtime = runtime;
        enhancedGpuMotionBlur.device = device;
        enhancedGpuMotionBlur.width = sourceDesc.texture.width;
        enhancedGpuMotionBlur.height = sourceDesc.texture.height;
        enhancedGpuMotionBlur.sourceFormat = sourceFormat;

        const auto sourceUsage = reshade::api::resource_usage::copy_dest |
                                 reshade::api::resource_usage::shader_resource;
        const auto accumulationUsage = reshade::api::resource_usage::render_target |
                                       reshade::api::resource_usage::shader_resource;
        const auto finalUsage = reshade::api::resource_usage::render_target |
                                reshade::api::resource_usage::copy_source;
        constexpr reshade::api::format accumulationFormat =
            reshade::api::format::r16g16b16a16_float;

        bool ok = device->create_resource(
            reshade::api::resource_desc(enhancedGpuMotionBlur.width, enhancedGpuMotionBlur.height,
                1, 1, sourceFormat, 1, reshade::api::memory_heap::gpu_only, sourceUsage),
            nullptr, reshade::api::resource_usage::shader_resource,
            &enhancedGpuMotionBlur.sourceCopy);
        ok = ok && device->create_resource(
            reshade::api::resource_desc(enhancedGpuMotionBlur.width, enhancedGpuMotionBlur.height,
                1, 1, accumulationFormat, 1, reshade::api::memory_heap::gpu_only, accumulationUsage),
            nullptr, reshade::api::resource_usage::shader_resource,
            &enhancedGpuMotionBlur.accumulation);
        ok = ok && device->create_resource(
            reshade::api::resource_desc(enhancedGpuMotionBlur.width, enhancedGpuMotionBlur.height,
                1, 1, sourceFormat, 1, reshade::api::memory_heap::gpu_only, finalUsage),
            nullptr, reshade::api::resource_usage::copy_source,
            &enhancedGpuMotionBlur.finalOutput);
        ok = ok && device->create_resource_view(
            enhancedGpuMotionBlur.sourceCopy, reshade::api::resource_usage::shader_resource,
            reshade::api::resource_view_desc(sourceFormat), &enhancedGpuMotionBlur.sourceSrv);
        ok = ok && device->create_resource_view(
            enhancedGpuMotionBlur.accumulation, reshade::api::resource_usage::shader_resource,
            reshade::api::resource_view_desc(accumulationFormat), &enhancedGpuMotionBlur.accumulationSrv);
        ok = ok && device->create_resource_view(
            enhancedGpuMotionBlur.accumulation, reshade::api::resource_usage::render_target,
            reshade::api::resource_view_desc(accumulationFormat), &enhancedGpuMotionBlur.accumulationRtv);
        ok = ok && device->create_resource_view(
            enhancedGpuMotionBlur.finalOutput, reshade::api::resource_usage::render_target,
            reshade::api::resource_view_desc(sourceFormat), &enhancedGpuMotionBlur.finalRtv);

        const reshade::api::pipeline_layout_param layoutParams[2] = {
            reshade::api::descriptor_range { 0, 0, 0, 1, reshade::api::shader_stage::pixel,
                                             1, reshade::api::descriptor_type::shader_resource_view },
            reshade::api::constant_range { 0, 0, 0, 4, reshade::api::shader_stage::pixel }
        };
        ok = ok && device->create_pipeline_layout(2, layoutParams,
                                                   &enhancedGpuMotionBlur.pipelineLayout);

        reshade::api::shader_desc vsDesc = {
            VSFullScreenEnhanced::g_main, sizeof(VSFullScreenEnhanced::g_main) };
        reshade::api::shader_desc accumulatePsDesc = {
            PSAccumulateEnhanced::g_main, sizeof(PSAccumulateEnhanced::g_main) };
        reshade::api::shader_desc dividePsDesc = {
            PSDivideEnhanced::g_main, sizeof(PSDivideEnhanced::g_main) };
        reshade::api::primitive_topology topology = reshade::api::primitive_topology::triangle_strip;
        reshade::api::rasterizer_desc rasterizer{};
        rasterizer.cull_mode = reshade::api::cull_mode::none;
        reshade::api::depth_stencil_desc depthStencil{};
        depthStencil.depth_enable = false;
        depthStencil.depth_write_mask = false;
        reshade::api::blend_desc additiveBlend{};
        additiveBlend.blend_enable[0] = true;
        additiveBlend.source_color_blend_factor[0] = reshade::api::blend_factor::one;
        additiveBlend.dest_color_blend_factor[0] = reshade::api::blend_factor::one;
        additiveBlend.source_alpha_blend_factor[0] = reshade::api::blend_factor::one;
        additiveBlend.dest_alpha_blend_factor[0] = reshade::api::blend_factor::one;

        std::vector<reshade::api::pipeline_subobject> accumulateSubobjects = {
            { reshade::api::pipeline_subobject_type::vertex_shader, 1, &vsDesc },
            { reshade::api::pipeline_subobject_type::pixel_shader, 1, &accumulatePsDesc },
            { reshade::api::pipeline_subobject_type::blend_state, 1, &additiveBlend },
            { reshade::api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer },
            { reshade::api::pipeline_subobject_type::depth_stencil_state, 1, &depthStencil },
            { reshade::api::pipeline_subobject_type::primitive_topology, 1, &topology },
            { reshade::api::pipeline_subobject_type::render_target_formats, 1,
              const_cast<reshade::api::format*>(&accumulationFormat) }
        };
        ok = ok && device->create_pipeline(
            enhancedGpuMotionBlur.pipelineLayout,
            static_cast<uint32_t>(accumulateSubobjects.size()), accumulateSubobjects.data(),
            &enhancedGpuMotionBlur.accumulatePipeline);

        reshade::api::blend_desc opaqueBlend{};
        std::vector<reshade::api::pipeline_subobject> divideSubobjects = {
            { reshade::api::pipeline_subobject_type::vertex_shader, 1, &vsDesc },
            { reshade::api::pipeline_subobject_type::pixel_shader, 1, &dividePsDesc },
            { reshade::api::pipeline_subobject_type::blend_state, 1, &opaqueBlend },
            { reshade::api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer },
            { reshade::api::pipeline_subobject_type::depth_stencil_state, 1, &depthStencil },
            { reshade::api::pipeline_subobject_type::primitive_topology, 1, &topology },
            { reshade::api::pipeline_subobject_type::render_target_formats, 1,
              &enhancedGpuMotionBlur.sourceFormat }
        };
        ok = ok && device->create_pipeline(
            enhancedGpuMotionBlur.pipelineLayout,
            static_cast<uint32_t>(divideSubobjects.size()), divideSubobjects.data(),
            &enhancedGpuMotionBlur.dividePipeline);

        if (!ok) {
            LOG(LL_ERR, "Failed to create Enhanced D3D12 GPU motion-blur resources; using CPU fallback");
            destroyEnhancedGpuMotionBlurResourcesLocked();
            enhancedGpuMotionBlur.runtime = runtime;
            enhancedGpuMotionBlur.device = device;
            enhancedGpuMotionBlur.width = sourceDesc.texture.width;
            enhancedGpuMotionBlur.height = sourceDesc.texture.height;
            enhancedGpuMotionBlur.sourceFormat = sourceFormat;
            enhancedGpuMotionBlur.creationFailed = true;
            return false;
        }

        LOG(LL_NFO, "Enhanced D3D12 GPU motion blur initialized. resolution=",
            enhancedGpuMotionBlur.width, "x", enhancedGpuMotionBlur.height,
            " accumulationFormat=RGBA16F outputFormat=", static_cast<uint32_t>(sourceFormat));
        return true;
    }

    void destroyEnhancedReadbackResourcesLocked() {
        if (enhancedReadbackDevice != nullptr) {
            for (auto& slot : enhancedReadbackSlots) {
                if (slot.resource.handle != 0) enhancedReadbackDevice->destroy_resource(slot.resource);
                if (slot.depthResource.handle != 0) enhancedReadbackDevice->destroy_resource(slot.depthResource);
                if (slot.fence.handle != 0) enhancedReadbackDevice->destroy_fence(slot.fence);
                slot = {};
            }
        } else {
            for (auto& slot : enhancedReadbackSlots) slot = {};
        }
        enhancedReadbackOrder.clear();
        enhancedReadbackCursor = 0;
        enhancedReadbackRuntime = nullptr;
        enhancedReadbackDevice = nullptr;
        enhancedReadbackWidth = 0;
        enhancedReadbackHeight = 0;
        enhancedReadbackFormat = reshade::api::format::unknown;
        enhancedReadbackDepthSourceFormat = reshade::api::format::unknown;
        enhancedReadbackDepthFormat = reshade::api::format::unknown;
        enhancedReadbackHasDepth = false;
    }

    bool ensureEnhancedReadbackResourcesLocked(reshade::api::effect_runtime* runtime,
                                               const reshade::api::resource_desc& sourceDesc,
                                               const reshade::api::resource_desc* depthDesc) {
        reshade::api::device* const device = runtime->get_device();
        const reshade::api::format format =
            reshade::api::format_to_default_typed(sourceDesc.texture.format, 0);
        const bool wantsDepth = Config::Manager::export_openexr;
        const bool hasDepth = wantsDepth && depthDesc != nullptr &&
            depthDesc->texture.width == sourceDesc.texture.width &&
            depthDesc->texture.height == sourceDesc.texture.height &&
            depthDesc->texture.samples == 1;
        const reshade::api::format depthSourceFormat = hasDepth
            ? depthDesc->texture.format
            : reshade::api::format::unknown;
        const reshade::api::format depthFormat = hasDepth
            ? enhancedDepthReadbackPlaneFormat(depthSourceFormat)
            : reshade::api::format::unknown;

        if (wantsDepth && !hasDepth) {
            if (!hasLoggedEnhancedMissingDepth.exchange(true)) {
                LOG(LL_ERR, "Enhanced OpenEXR requires a full-resolution single-sample depth buffer, but none is active");
            }
            return false;
        }
        if (hasDepth && depthFormat == reshade::api::format::unknown) {
            LOG(LL_ERR, "Enhanced OpenEXR depth format has no supported readback plane. sourceFormat=",
                static_cast<uint32_t>(depthSourceFormat));
            return false;
        }

        if (enhancedReadbackRuntime == runtime && enhancedReadbackDevice == device &&
            enhancedReadbackWidth == sourceDesc.texture.width &&
            enhancedReadbackHeight == sourceDesc.texture.height &&
            enhancedReadbackFormat == format &&
            enhancedReadbackHasDepth == hasDepth &&
            enhancedReadbackDepthSourceFormat == depthSourceFormat &&
            enhancedReadbackDepthFormat == depthFormat &&
            enhancedReadbackSlots[0].resource.handle != 0) {
            return true;
        }

        destroyEnhancedReadbackResourcesLocked();
        enhancedReadbackRuntime = runtime;
        enhancedReadbackDevice = device;
        enhancedReadbackWidth = sourceDesc.texture.width;
        enhancedReadbackHeight = sourceDesc.texture.height;
        enhancedReadbackFormat = format;
        enhancedReadbackDepthSourceFormat = depthSourceFormat;
        enhancedReadbackDepthFormat = depthFormat;
        enhancedReadbackHasDepth = hasDepth;

        for (auto& slot : enhancedReadbackSlots) {
            if (!device->create_resource(
                    reshade::api::resource_desc(enhancedReadbackWidth, enhancedReadbackHeight, 1, 1,
                        enhancedReadbackFormat, 1, reshade::api::memory_heap::gpu_to_cpu,
                        reshade::api::resource_usage::copy_dest),
                    nullptr, reshade::api::resource_usage::copy_dest, &slot.resource) ||
                !device->create_fence(0, reshade::api::fence_flags::none, &slot.fence)) {
                LOG(LL_ERR, "Failed to create Enhanced asynchronous readback ring");
                destroyEnhancedReadbackResourcesLocked();
                return false;
            }
            if (hasDepth && !device->create_resource(
                    reshade::api::resource_desc(enhancedReadbackWidth, enhancedReadbackHeight, 1, 1,
                        enhancedReadbackDepthFormat, 1, reshade::api::memory_heap::gpu_to_cpu,
                        reshade::api::resource_usage::copy_dest),
                    nullptr, reshade::api::resource_usage::copy_dest, &slot.depthResource)) {
                LOG(LL_ERR, "Failed to create Enhanced asynchronous depth readback ring");
                destroyEnhancedReadbackResourcesLocked();
                return false;
            }
        }

        LOG(LL_NFO, "Enhanced asynchronous D3D12 readback ring created. slots=",
            kEnhancedReadbackSlotCount, " resolution=", enhancedReadbackWidth, "x", enhancedReadbackHeight,
            " format=", static_cast<uint32_t>(enhancedReadbackFormat),
            " depth=", enhancedReadbackHasDepth,
            " depthSourceFormat=", static_cast<uint32_t>(enhancedReadbackDepthSourceFormat),
            " depthPlaneFormat=", static_cast<uint32_t>(enhancedReadbackDepthFormat));
        return true;
    }

    bool queueEnhancedReadbackLocked(reshade::api::effect_runtime* runtime,
                                     reshade::api::resource source,
                                     reshade::api::resource_usage sourceState,
                                     reshade::api::resource depthSource,
                                     reshade::api::resource_usage depthState,
                                     uint64_t generation,
                                     bool gpuMotionBlurOutput = false) {
        for (size_t attempt = 0; attempt < kEnhancedReadbackSlotCount; ++attempt) {
            const size_t index = (enhancedReadbackCursor + attempt) % kEnhancedReadbackSlotCount;
            auto& slot = enhancedReadbackSlots[index];
            if (slot.pending) continue;

            reshade::api::command_queue* const queue = runtime->get_command_queue();
            reshade::api::command_list* const commandList =
                queue != nullptr ? queue->get_immediate_command_list() : nullptr;
            if (queue == nullptr || commandList == nullptr) return false;

            if (sourceState != reshade::api::resource_usage::copy_source)
                commandList->barrier(source, sourceState, reshade::api::resource_usage::copy_source);
            commandList->copy_texture_region(source, 0, nullptr, slot.resource, 0, nullptr);
            if (sourceState != reshade::api::resource_usage::copy_source)
                commandList->barrier(source, reshade::api::resource_usage::copy_source, sourceState);

            if (enhancedReadbackHasDepth) {
                if (depthSource.handle == 0 || slot.depthResource.handle == 0) {
                    LOG(LL_ERR, "Enhanced depth readback ring lost its active depth resource");
                    return false;
                }
                commandList->barrier(depthSource, depthState, reshade::api::resource_usage::copy_source);
                commandList->copy_texture_region(depthSource, 0, nullptr, slot.depthResource, 0, nullptr);
                commandList->barrier(depthSource, reshade::api::resource_usage::copy_source, depthState);
            }

            const uint64_t fenceValue = slot.fenceValue + 1;
            if (!queue->signal(slot.fence, fenceValue)) {
                LOG(LL_ERR, "Failed to signal Enhanced asynchronous readback fence");
                return false;
            }

            slot.fenceValue = fenceValue;
            slot.generation = generation;
            slot.gpuMotionBlurOutput = gpuMotionBlurOutput;
            slot.pending = true;
            enhancedReadbackOrder.push_back(index);
            enhancedReadbackCursor = (index + 1) % kEnhancedReadbackSlotCount;
            return true;
        }
        return false;
    }

    bool consumeEnhancedReadbackLocked(std::vector<uint8_t>& pixels,
                                       std::vector<float>& depth,
                                       uint64_t& generation,
                                       bool& gpuMotionBlurOutput) {
        if (enhancedReadbackOrder.empty() || enhancedReadbackDevice == nullptr) return false;

        const size_t index = enhancedReadbackOrder.front();
        auto& slot = enhancedReadbackSlots[index];
        if (!slot.pending ||
            enhancedReadbackDevice->get_completed_fence_value(slot.fence) < slot.fenceValue) {
            return false;
        }

        reshade::api::subresource_data mapped{};
        bool copied = false;
        if (enhancedReadbackDevice->map_texture_region(
                slot.resource, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
            copied = copyEnhancedMappedPixels(mapped, enhancedReadbackFormat,
                                               enhancedReadbackWidth, enhancedReadbackHeight, pixels);
            enhancedReadbackDevice->unmap_texture_region(slot.resource, 0);
        }

        bool copiedDepth = !enhancedReadbackHasDepth;
        if (copied && enhancedReadbackHasDepth && slot.depthResource.handle != 0) {
            reshade::api::subresource_data mappedDepth{};
            if (enhancedReadbackDevice->map_texture_region(
                    slot.depthResource, 0, nullptr, reshade::api::map_access::read_only, &mappedDepth)) {
                copiedDepth = copyEnhancedMappedDepth(mappedDepth, enhancedReadbackDepthFormat,
                                                      enhancedReadbackWidth, enhancedReadbackHeight, depth);
                enhancedReadbackDevice->unmap_texture_region(slot.depthResource, 0);
            }
        } else if (!enhancedReadbackHasDepth) {
            depth.clear();
        }

        generation = slot.generation;
        gpuMotionBlurOutput = slot.gpuMotionBlurOutput;
        slot.pending = false;
        enhancedReadbackOrder.pop_front();
        if (!copied || !copiedDepth) {
            LOG(LL_ERR, "Failed to map or convert Enhanced asynchronous color/depth readback frame");
        }
        return copied && copiedDepth;
    }

    bool hasFreeEnhancedReadbackSlotLocked() {
        return std::any_of(enhancedReadbackSlots.begin(), enhancedReadbackSlots.end(),
                           [](const EnhancedReadbackSlot& slot) { return !slot.pending; });
    }

    HRESULT processEnhancedGpuMotionBlurOutput(const uint8_t* pixels,
                                               uint32_t rowPitch,
                                               uint32_t height) {
        if (pixels == nullptr || rowPitch == 0 || height == 0 || encodingSession == nullptr)
            return E_INVALIDARG;
        if (enhancedPendingVideoFrames.load(std::memory_order_acquire) == 0)
            return S_FALSE;

        const HRESULT result = encodingSession->enqueueVideoFrame(pixels, rowPitch, height);
        if (SUCCEEDED(result)) {
            ++enhancedGpuMotionBlur.emittedFrames;
            enhancedPendingVideoFrames.fetch_sub(1, std::memory_order_release);
        }
        return result;
    }

    bool processEnhancedGpuMotionBlurSubframeLocked(
        reshade::api::effect_runtime* runtime,
        reshade::api::resource source,
        reshade::api::resource_usage sourceState,
        uint64_t generation) {
        if (runtime == nullptr || enhancedGpuMotionBlur.sourceCopy.handle == 0 ||
            enhancedPendingVideoFrames.load(std::memory_order_acquire) == 0) {
            return false;
        }

        const uint8_t sampleCount = Config::Manager::motion_blur_samples;
        const float strength = std::clamp(Config::Manager::motion_blur_strength, 0.0f, 1.0f);
        if (sampleCount == 0) return false;

        const bool configurationChanged =
            enhancedGpuMotionBlur.configuredSamples != sampleCount ||
            enhancedGpuMotionBlur.configuredStrength != strength;
        if (configurationChanged) {
            enhancedGpuMotionBlur.subframeIndex = 0;
            enhancedGpuMotionBlur.accumulatedSamples = 0;
            enhancedGpuMotionBlur.configuredSamples = sampleCount;
            enhancedGpuMotionBlur.configuredStrength = strength;
            enhancedGpuMotionBlur.consumedSubframes = 0;
            enhancedGpuMotionBlur.emittedFrames = 0;
            LOG(LL_NFO, "Enhanced GPU multi-sample motion blur active. samples=",
                static_cast<uint32_t>(sampleCount), " strength=", strength,
                " groupSize=", static_cast<uint32_t>(sampleCount) + 1,
                " resolution=", enhancedGpuMotionBlur.width, "x", enhancedGpuMotionBlur.height,
                " readbacksPerOutputFrame=1");
        }

        const bool groupComplete = enhancedGpuMotionBlur.subframeIndex == sampleCount;
        if (groupComplete && !hasFreeEnhancedReadbackSlotLocked()) return false;

        auto* const queue = runtime->get_command_queue();
        auto* const commandList = queue != nullptr ? queue->get_immediate_command_list() : nullptr;
        if (queue == nullptr || commandList == nullptr) return false;

        const float shutterPosition =
            static_cast<float>(enhancedGpuMotionBlur.subframeIndex) / static_cast<float>(sampleCount);
        const bool includeSample = shutterPosition >= (1.0f - strength);
        if (includeSample) {
            commandList->barrier(source, sourceState, reshade::api::resource_usage::copy_source);
            commandList->barrier(enhancedGpuMotionBlur.sourceCopy,
                                 reshade::api::resource_usage::shader_resource,
                                 reshade::api::resource_usage::copy_dest);
            commandList->copy_texture_region(source, 0, nullptr,
                                             enhancedGpuMotionBlur.sourceCopy, 0, nullptr);
            commandList->barrier(enhancedGpuMotionBlur.sourceCopy,
                                 reshade::api::resource_usage::copy_dest,
                                 reshade::api::resource_usage::shader_resource);
            commandList->barrier(source, reshade::api::resource_usage::copy_source, sourceState);

            commandList->barrier(enhancedGpuMotionBlur.accumulation,
                                 reshade::api::resource_usage::shader_resource,
                                 reshade::api::resource_usage::render_target);
            if (enhancedGpuMotionBlur.accumulatedSamples == 0) {
                constexpr float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                commandList->clear_render_target_view(
                    enhancedGpuMotionBlur.accumulationRtv, clearColor);
            }

            const reshade::api::viewport viewport = {
                0.0f, 0.0f, static_cast<float>(enhancedGpuMotionBlur.width),
                static_cast<float>(enhancedGpuMotionBlur.height), 0.0f, 1.0f };
            const reshade::api::rect scissor = {
                0, 0, static_cast<int32_t>(enhancedGpuMotionBlur.width),
                static_cast<int32_t>(enhancedGpuMotionBlur.height) };
            commandList->bind_pipeline(reshade::api::pipeline_stage::all_graphics,
                                       enhancedGpuMotionBlur.accumulatePipeline);
            commandList->push_descriptors(
                reshade::api::shader_stage::pixel, enhancedGpuMotionBlur.pipelineLayout, 0,
                reshade::api::descriptor_table_update {
                    {}, 0, 0, 1, reshade::api::descriptor_type::shader_resource_view,
                    &enhancedGpuMotionBlur.sourceSrv });
            commandList->bind_viewports(0, 1, &viewport);
            commandList->bind_scissor_rects(0, 1, &scissor);
            commandList->bind_render_targets_and_depth_stencil(
                1, &enhancedGpuMotionBlur.accumulationRtv);
            commandList->draw(4, 1, 0, 0);
            commandList->barrier(enhancedGpuMotionBlur.accumulation,
                                 reshade::api::resource_usage::render_target,
                                 reshade::api::resource_usage::shader_resource);
            ++enhancedGpuMotionBlur.accumulatedSamples;
        }

        if (groupComplete) {
            if (enhancedGpuMotionBlur.accumulatedSamples == 0) {
                LOG(LL_ERR, "Enhanced GPU motion-blur group has no accumulated samples");
                return false;
            }

            const float divideValues[4] = {
                static_cast<float>(enhancedGpuMotionBlur.accumulatedSamples), 0.0f, 0.0f, 0.0f };
            const reshade::api::viewport viewport = {
                0.0f, 0.0f, static_cast<float>(enhancedGpuMotionBlur.width),
                static_cast<float>(enhancedGpuMotionBlur.height), 0.0f, 1.0f };
            const reshade::api::rect scissor = {
                0, 0, static_cast<int32_t>(enhancedGpuMotionBlur.width),
                static_cast<int32_t>(enhancedGpuMotionBlur.height) };
            commandList->barrier(enhancedGpuMotionBlur.finalOutput,
                                 reshade::api::resource_usage::copy_source,
                                 reshade::api::resource_usage::render_target);
            commandList->bind_pipeline(reshade::api::pipeline_stage::all_graphics,
                                       enhancedGpuMotionBlur.dividePipeline);
            commandList->push_descriptors(
                reshade::api::shader_stage::pixel, enhancedGpuMotionBlur.pipelineLayout, 0,
                reshade::api::descriptor_table_update {
                    {}, 0, 0, 1, reshade::api::descriptor_type::shader_resource_view,
                    &enhancedGpuMotionBlur.accumulationSrv });
            commandList->push_constants(reshade::api::shader_stage::pixel,
                                        enhancedGpuMotionBlur.pipelineLayout, 1, 0, 4, divideValues);
            commandList->bind_viewports(0, 1, &viewport);
            commandList->bind_scissor_rects(0, 1, &scissor);
            commandList->bind_render_targets_and_depth_stencil(1, &enhancedGpuMotionBlur.finalRtv);
            commandList->draw(4, 1, 0, 0);
            commandList->barrier(enhancedGpuMotionBlur.finalOutput,
                                 reshade::api::resource_usage::render_target,
                                 reshade::api::resource_usage::copy_source);

            if (!queueEnhancedReadbackLocked(
                    runtime, enhancedGpuMotionBlur.finalOutput,
                    reshade::api::resource_usage::copy_source, {},
                    reshade::api::resource_usage::undefined, generation, true)) {
                LOG(LL_ERR, "Failed to queue Enhanced GPU motion-blur output readback");
                return false;
            }

            enhancedGpuMotionBlur.subframeIndex = 0;
            enhancedGpuMotionBlur.accumulatedSamples = 0;
        } else {
            ++enhancedGpuMotionBlur.subframeIndex;
            enhancedPendingVideoFrames.fetch_sub(1, std::memory_order_release);
        }

        ++enhancedGpuMotionBlur.consumedSubframes;
        return true;
    }

    HRESULT processEnhancedCapturedVideoSample(const uint8_t* pixels, uint32_t rowPitch, uint32_t height,
                                               const float* depth, uint32_t depthRowPitch) {
        if (pixels == nullptr || rowPitch == 0 || height == 0 || encodingSession == nullptr) {
            return E_INVALIDARG;
        }
        if (enhancedPendingVideoFrames.load(std::memory_order_acquire) == 0) {
            return S_FALSE;
        }

        const uint8_t sampleCount = Config::Manager::motion_blur_samples;
        const bool useMotionBlur = isCustomFrameRateSupported && sampleCount != 0;
        if (!useMotionBlur) {
            const HRESULT enqueueHr = encodingSession->enqueueVideoFrame(pixels, rowPitch, height);
            if (SUCCEEDED(enqueueHr)) {
                enhancedPendingVideoFrames.fetch_sub(1, std::memory_order_release);
                if (Config::Manager::export_openexr) {
                    if (depth == nullptr || depthRowPitch == 0) {
                        LOG(LL_ERR, "Enhanced OpenEXR frame is missing its synchronized depth plane");
                        return E_FAIL;
                    }
                    return encodingSession->enqueueExrImage(pixels, rowPitch, depth, depthRowPitch);
                }
            }
            return enqueueHr;
        }

        const float strength = std::clamp(Config::Manager::motion_blur_strength, 0.0f, 1.0f);
        const size_t frameBytes = static_cast<size_t>(rowPitch) * height;
        const bool configurationChanged =
            enhancedMotionBlur.rowPitch != rowPitch ||
            enhancedMotionBlur.height != height ||
            enhancedMotionBlur.configuredSamples != sampleCount ||
            enhancedMotionBlur.configuredStrength != strength;
        if (configurationChanged) {
            enhancedMotionBlur = {};
            enhancedMotionBlur.rowPitch = rowPitch;
            enhancedMotionBlur.height = height;
            enhancedMotionBlur.configuredSamples = sampleCount;
            enhancedMotionBlur.configuredStrength = strength;
            enhancedMotionBlur.accumulation.assign(frameBytes, 0);
            enhancedMotionBlur.output.resize(frameBytes);
            LOG(LL_NFO, "Enhanced multi-sample motion blur active. samples=", static_cast<uint32_t>(sampleCount),
                " strength=", strength, " groupSize=", static_cast<uint32_t>(sampleCount) + 1,
                " resolution=", rowPitch / 4, "x", height);
        }

        const float shutterPosition =
            static_cast<float>(enhancedMotionBlur.subframeIndex) / static_cast<float>(sampleCount);
        if (shutterPosition >= (1.0f - strength)) {
            parallelForMotionBlurBytes(frameBytes, [&](const size_t begin, const size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    enhancedMotionBlur.accumulation[i] = static_cast<uint16_t>(
                        enhancedMotionBlur.accumulation[i] + pixels[i]);
                }
            });
            ++enhancedMotionBlur.accumulatedSamples;
        }

        HRESULT result = S_OK;
        const bool groupComplete = enhancedMotionBlur.subframeIndex == sampleCount;
        if (groupComplete) {
            if (enhancedMotionBlur.accumulatedSamples == 0) {
                LOG(LL_ERR, "Enhanced motion blur group completed without an accumulated sample");
                return E_UNEXPECTED;
            }

            const uint32_t divisor = enhancedMotionBlur.accumulatedSamples;
            parallelForMotionBlurBytes(frameBytes, [&](const size_t begin, const size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    enhancedMotionBlur.output[i] = static_cast<uint8_t>(
                        (static_cast<uint32_t>(enhancedMotionBlur.accumulation[i]) + divisor / 2) / divisor);
                    enhancedMotionBlur.accumulation[i] = 0;
                }
            });
            result = encodingSession->enqueueVideoFrame(enhancedMotionBlur.output.data(), rowPitch, height);
            if (FAILED(result)) return result;

            if (Config::Manager::export_openexr) {
                if (depth == nullptr || depthRowPitch == 0) {
                    LOG(LL_ERR, "Enhanced motion-blur EXR frame is missing its synchronized depth plane");
                    return E_FAIL;
                }
                result = encodingSession->enqueueExrImage(
                    enhancedMotionBlur.output.data(), rowPitch, depth, depthRowPitch);
                if (FAILED(result)) return result;
            }

            ++enhancedMotionBlur.emittedFrames;
            enhancedMotionBlur.subframeIndex = 0;
            enhancedMotionBlur.accumulatedSamples = 0;
        } else {
            ++enhancedMotionBlur.subframeIndex;
        }

        ++enhancedMotionBlur.consumedSubframes;
        enhancedPendingVideoFrames.fetch_sub(1, std::memory_order_release);
        return result;
    }

    HRESULT drainEnhancedReadbacksForFinalize() {
        std::lock_guard<std::mutex> readbackLock(enhancedReadbackMutex);
        if (enhancedReadbackDevice == nullptr || enhancedReadbackOrder.empty()) {
            return enhancedPendingVideoFrames.load(std::memory_order_acquire) == 0 ? S_OK : E_FAIL;
        }

        const uint64_t activeGeneration = enhancedCaptureGeneration.load(std::memory_order_acquire);
        const uint32_t width = enhancedReadbackWidth;
        const uint32_t height = enhancedReadbackHeight;
        const size_t rowPitch = static_cast<size_t>(width) * 4;
        size_t drainedFrames = 0;

        while (!enhancedReadbackOrder.empty()) {
            const auto& front = enhancedReadbackSlots[enhancedReadbackOrder.front()];
            if (!enhancedReadbackDevice->wait(front.fence, front.fenceValue, 2'000'000'000ULL)) {
                LOG(LL_ERR, "Timed out draining Enhanced D3D12 readback during finalization");
                break;
            }

            uint64_t completedGeneration = 0;
            bool gpuMotionBlurOutput = false;
            if (!consumeEnhancedReadbackLocked(
                    reshadeCapturePixels, enhancedCaptureDepth, completedGeneration,
                    gpuMotionBlurOutput)) break;
            if (completedGeneration != activeGeneration ||
                enhancedPendingVideoFrames.load(std::memory_order_acquire) == 0) continue;

            const HRESULT enqueueHr = gpuMotionBlurOutput
                ? processEnhancedGpuMotionBlurOutput(
                    reshadeCapturePixels.data(), static_cast<uint32_t>(rowPitch), height)
                : processEnhancedCapturedVideoSample(
                    reshadeCapturePixels.data(), static_cast<uint32_t>(rowPitch), height,
                    enhancedCaptureDepth.empty() ? nullptr : enhancedCaptureDepth.data(),
                    enhancedCaptureDepth.empty() ? 0u : width * static_cast<uint32_t>(sizeof(float)));
            if (FAILED(enqueueHr)) return enqueueHr;
            ++drainedFrames;
        }

        const uint32_t remaining = enhancedPendingVideoFrames.load(std::memory_order_acquire);
        LOG(remaining == 0 ? LL_NFO : LL_ERR,
            "Enhanced final readback drain complete. drainedFrames=", drainedFrames,
            " remainingVideoSamples=", remaining);
        if (enhancedMotionBlur.configuredSamples != 0) {
            LOG(remaining == 0 && enhancedMotionBlur.subframeIndex == 0 ? LL_NFO : LL_WRN,
                "Enhanced motion blur summary. consumedSubframes=", enhancedMotionBlur.consumedSubframes,
                " emittedFrames=", enhancedMotionBlur.emittedFrames,
                " groupSize=", static_cast<uint32_t>(enhancedMotionBlur.configuredSamples) + 1,
                " partialGroupSubframes=", enhancedMotionBlur.subframeIndex,
                " currentGroupAccumulatedSamples=", enhancedMotionBlur.accumulatedSamples);
        }
        if (enhancedGpuMotionBlur.configuredSamples != 0) {
            LOG(remaining == 0 && enhancedGpuMotionBlur.subframeIndex == 0 ? LL_NFO : LL_WRN,
                "Enhanced GPU motion blur summary. consumedSubframes=",
                enhancedGpuMotionBlur.consumedSubframes,
                " emittedFrames=", enhancedGpuMotionBlur.emittedFrames,
                " groupSize=", static_cast<uint32_t>(enhancedGpuMotionBlur.configuredSamples) + 1,
                " partialGroupSubframes=", enhancedGpuMotionBlur.subframeIndex,
                " currentGroupAccumulatedSamples=", enhancedGpuMotionBlur.accumulatedSamples);
        }
        return remaining == 0 ? S_OK : E_FAIL;
    }

    bool captureEnhancedResource(reshade::api::effect_runtime* runtime,
                                 reshade::api::resource source,
                                 reshade::api::resource_usage sourceState,
                                 std::vector<uint8_t>& pixels,
                                 uint32_t& width,
                                 uint32_t& height) {
        reshade::api::device* const device = runtime->get_device();
        const reshade::api::resource_desc desc = device->get_resource_desc(source);
        const reshade::api::format viewFormat =
            reshade::api::format_to_default_typed(desc.texture.format, 0);

        switch (viewFormat) {
        case reshade::api::format::r8g8b8a8_unorm:
        case reshade::api::format::r8g8b8x8_unorm:
        case reshade::api::format::b8g8r8a8_unorm:
        case reshade::api::format::b8g8r8x8_unorm:
        case reshade::api::format::r10g10b10a2_unorm:
        case reshade::api::format::b10g10r10a2_unorm:
            break;
        default:
            LOG(LL_ERR, "Enhanced VideoEncode resource uses an unsupported capture format: ",
                static_cast<uint32_t>(desc.texture.format));
            return false;
        }

        width = desc.texture.width;
        height = desc.texture.height;
        if (width == 0 || height == 0) return false;

        reshade::api::resource readback{};
        if (!device->create_resource(
                reshade::api::resource_desc(width, height, 1, 1, viewFormat, 1,
                    reshade::api::memory_heap::gpu_to_cpu,
                    reshade::api::resource_usage::copy_dest),
                nullptr, reshade::api::resource_usage::copy_dest, &readback)) {
            LOG(LL_ERR, "Failed to create Enhanced VideoEncode readback resource");
            return false;
        }

        bool succeeded = false;
        reshade::api::command_queue* const queue = runtime->get_command_queue();
        reshade::api::command_list* const commandList = queue->get_immediate_command_list();
        const reshade::api::resource_usage effectiveState =
            sourceState == reshade::api::resource_usage::undefined
                ? reshade::api::resource_usage::render_target
                : sourceState;

        if (effectiveState != reshade::api::resource_usage::copy_source)
            commandList->barrier(source, effectiveState, reshade::api::resource_usage::copy_source);
        commandList->copy_texture_region(source, 0, nullptr, readback, 0, nullptr);
        if (effectiveState != reshade::api::resource_usage::copy_source)
            commandList->barrier(source, reshade::api::resource_usage::copy_source, effectiveState);

        reshade::api::fence copyFence{};
        const bool fenceCreated = device->create_fence(0, reshade::api::fence_flags::none, &copyFence);
        if (!fenceCreated || !queue->signal(copyFence, 1) || !device->wait(copyFence, 1))
            queue->wait_idle();
        if (fenceCreated) device->destroy_fence(copyFence);

        reshade::api::subresource_data mapped{};
        if (device->map_texture_region(readback, 0, nullptr,
                reshade::api::map_access::read_only, &mapped)) {
            const size_t outputRowPitch = static_cast<size_t>(width) * 4;
            pixels.resize(outputRowPitch * static_cast<size_t>(height));
            auto* destination = pixels.data();
            auto* sourcePixels = static_cast<const uint8_t*>(mapped.data);

            for (uint32_t y = 0; y < height; ++y,
                 destination += outputRowPitch, sourcePixels += mapped.row_pitch) {
                switch (viewFormat) {
                case reshade::api::format::r8g8b8a8_unorm:
                case reshade::api::format::r8g8b8x8_unorm:
                    std::memcpy(destination, sourcePixels, outputRowPitch);
                    if (viewFormat == reshade::api::format::r8g8b8x8_unorm)
                        for (size_t x = 3; x < outputRowPitch; x += 4) destination[x] = 0xFF;
                    break;
                case reshade::api::format::b8g8r8a8_unorm:
                case reshade::api::format::b8g8r8x8_unorm:
                    std::memcpy(destination, sourcePixels, outputRowPitch);
                    for (size_t x = 0; x < outputRowPitch; x += 4) {
                        std::swap(destination[x], destination[x + 2]);
                        if (viewFormat == reshade::api::format::b8g8r8x8_unorm) destination[x + 3] = 0xFF;
                    }
                    break;
                case reshade::api::format::r10g10b10a2_unorm:
                case reshade::api::format::b10g10r10a2_unorm:
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint32_t packed = reinterpret_cast<const uint32_t*>(sourcePixels)[x];
                        destination[x * 4 + 0] = static_cast<uint8_t>((packed & 0x3FF) >> 2);
                        destination[x * 4 + 1] = static_cast<uint8_t>(((packed >> 10) & 0x3FF) >> 2);
                        destination[x * 4 + 2] = static_cast<uint8_t>(((packed >> 20) & 0x3FF) >> 2);
                        destination[x * 4 + 3] = static_cast<uint8_t>(((packed >> 30) & 0x3) * 85);
                        if (viewFormat == reshade::api::format::b10g10r10a2_unorm)
                            std::swap(destination[x * 4], destination[x * 4 + 2]);
                    }
                    break;
                default:
                    break;
                }
            }

            device->unmap_texture_region(readback, 0);
            succeeded = true;
        }

        device->destroy_resource(readback);
        return succeeded;
    }

    bool ensureD3D11Hook(bool allowLoadLibrary = true) {
        touchD3D11Import();

        HMODULE module = GetModuleHandleW(L"d3d11.dll");
        if (module == nullptr) {
            if (!allowLoadLibrary) {
                return false;
            }

            if (ImportHooks::LoadLibraryW::OriginalFunc != nullptr) {
                module = ImportHooks::LoadLibraryW::OriginalFunc(L"d3d11.dll");
            } else {
                module = ::LoadLibraryW(L"d3d11.dll");
            }
        }

        if (module == nullptr) {
            return false;
        }

        return installD3D11Hook(module);
    }
}

void ever::OnPresent(IDXGISwapChain* p_swap_chain) {
    captureMainSwapChain(p_swap_chain, "IDXGISwapChain::Present");
    installFactoryHooksFromSwapChain(p_swap_chain, "IDXGISwapChain::Present");
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        try {
            ComPtr<ID3D11Device> pDevice;
            ComPtr<ID3D11DeviceContext> pDeviceContext;
            ComPtr<ID3D11Texture2D> texture;
            DXGI_SWAP_CHAIN_DESC desc;

            REQUIRE(p_swap_chain->GetDesc(&desc), "Failed to get swap chain descriptor");

            LOG(LL_NFO, "BUFFER COUNT: ", desc.BufferCount);
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }
    }
}

bool ever::isEnhancedEdition() {
    return g_isEnhancedEdition;
}

bool ever::isCustomFrameRateAvailable() {
    return isCustomFrameRateSupported;
}

bool ever::isDualPassAvailable() {
    return !g_isEnhancedEdition || isEnhancedDualPassSupported;
}

void ever::OnReshadeInitRuntime(reshade::api::effect_runtime* runtime) {
    if (!g_isEnhancedEdition || runtime == nullptr || runtime->get_device() == nullptr ||
        runtime->get_device()->get_api() != reshade::api::device_api::d3d12) return;

    uint32_t width = 0;
    uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    if (width == 0 || height == 0) return;

    {
        std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
        const uint32_t backBufferCount = runtime->get_back_buffer_count();
        for (uint32_t index = 0; index < backBufferCount; ++index) {
            const reshade::api::resource backBuffer = runtime->get_back_buffer(index);
            if (backBuffer.handle != 0) enhancedBackBufferHandles.insert(backBuffer.handle);
        }
    }

    const uint64_t currentArea = static_cast<uint64_t>(enhancedRuntimeWidth.load(std::memory_order_relaxed)) *
                                 enhancedRuntimeHeight.load(std::memory_order_relaxed);
    const uint64_t candidateArea = static_cast<uint64_t>(width) * height;
    if (candidateArea >= currentArea || enhancedRuntimeSource.load(std::memory_order_relaxed) == runtime) {
        enhancedRuntimeWidth.store(width, std::memory_order_relaxed);
        enhancedRuntimeHeight.store(height, std::memory_order_relaxed);
        enhancedRuntimeSource.store(runtime, std::memory_order_relaxed);
    }
}

void ever::OnReshadeDestroyRuntime(reshade::api::effect_runtime* runtime) {
    if (!g_isEnhancedEdition || runtime == nullptr) return;

    {
        std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
        const uint32_t backBufferCount = runtime->get_back_buffer_count();
        for (uint32_t index = 0; index < backBufferCount; ++index) {
            enhancedBackBufferHandles.erase(runtime->get_back_buffer(index).handle);
        }
    }

    {
        std::lock_guard<std::mutex> readbackLock(enhancedReadbackMutex);
        if (enhancedReadbackRuntime == runtime) destroyEnhancedReadbackResourcesLocked();
        if (enhancedGpuMotionBlur.runtime == runtime) destroyEnhancedGpuMotionBlurResourcesLocked();
    }

    if (enhancedRuntimeSource.load(std::memory_order_relaxed) == runtime) {
        enhancedRuntimeSource.store(nullptr, std::memory_order_relaxed);
        // Keep the last valid dimensions across ReShade's ResizeBuffers reset. The Enhanced
        // export creates its Media Foundation sink while the same runtime is being recreated,
        // so clearing these here makes the first export attempt fail spuriously.
    }
}

void ever::OnReshadeRenderTarget(reshade::api::device* device, reshade::api::resource_view view) {
    if (!g_isEnhancedEdition || device == nullptr || view.handle == 0 ||
        enhancedExpectedWidth.load(std::memory_order_relaxed) == 0) return;
    const reshade::api::resource resource = device->get_resource_from_view(view);
    tryTrackEnhancedVideoResource(device, resource, reshade::api::resource_usage::render_target);

    const reshade::api::resource_desc desc = device->get_resource_desc(resource);
    if (desc.texture.width != enhancedExpectedWidth.load(std::memory_order_relaxed) ||
        desc.texture.height != enhancedExpectedHeight.load(std::memory_order_relaxed)) return;

    bool shouldLog = false;
    bool isBackBuffer = false;
    {
        std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
        shouldLog = enhancedLoggedRenderTargetHandles.insert(resource.handle).second;
        isBackBuffer = enhancedBackBufferHandles.contains(resource.handle);
    }

    const reshade::api::format typedFormat =
        reshade::api::format_to_default_typed(desc.texture.format, 0);
    const bool isCleanColorCandidate = !isBackBuffer &&
        (typedFormat == reshade::api::format::r8g8b8a8_unorm ||
         typedFormat == reshade::api::format::r8g8b8x8_unorm ||
         typedFormat == reshade::api::format::b8g8r8a8_unorm ||
         typedFormat == reshade::api::format::b8g8r8x8_unorm);
    if (isCleanColorCandidate) {
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        enhancedCleanCandidateDevice = device;
        enhancedCleanCandidateResource = resource;
        enhancedCleanCandidateState = reshade::api::resource_usage::render_target;
    }

    if (shouldLog) {
        const std::string name = getD3D12ResourceDebugName(device, resource);
        LOG(LL_NFO, "Enhanced full-resolution render-target candidate. handle=",
            Logger::hex(resource.handle, 16), " backBuffer=", isBackBuffer,
            " format=", static_cast<uint32_t>(desc.texture.format),
            " name=", name.empty() ? "<unnamed>" : name);
    }
}

void ever::OnReshadeDepthStencil(reshade::api::device* device, reshade::api::resource_view view) {
    if (!g_isEnhancedEdition || device == nullptr || view.handle == 0 ||
        enhancedExpectedWidth.load(std::memory_order_relaxed) == 0) return;

    const reshade::api::resource resource = device->get_resource_from_view(view);
    if (resource.handle == 0) return;
    const reshade::api::resource_desc desc = device->get_resource_desc(resource);
    if (desc.texture.width != enhancedExpectedWidth.load(std::memory_order_relaxed) ||
        desc.texture.height != enhancedExpectedHeight.load(std::memory_order_relaxed)) return;

    const reshade::api::format depthFormat =
        reshade::api::format_to_depth_stencil_typed(desc.texture.format);
    const bool supportedDepthFormat =
        depthFormat == reshade::api::format::d16_unorm ||
        depthFormat == reshade::api::format::d24_unorm_s8_uint ||
        depthFormat == reshade::api::format::d32_float ||
        depthFormat == reshade::api::format::d32_float_s8_uint;
    if (!supportedDepthFormat) return;

    {
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        enhancedDepthCandidateDevice = device;
        enhancedDepthCandidateResource = resource;
        enhancedDepthCandidateState = reshade::api::resource_usage::depth_stencil_write;
    }

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
        shouldLog = enhancedLoggedDepthStencilHandles.insert(resource.handle).second;
    }
    if (shouldLog) {
        const std::string name = getD3D12ResourceDebugName(device, resource);
        LOG(LL_NFO, "Enhanced full-resolution depth-stencil candidate. handle=",
            Logger::hex(resource.handle, 16),
            " format=", static_cast<uint32_t>(desc.texture.format),
            " typedDepthFormat=", static_cast<uint32_t>(depthFormat),
            " samples=", desc.texture.samples,
            " name=", name.empty() ? "<unnamed>" : name);
    }
}

void ever::OnReshadeBarrier(reshade::api::device* device, uint32_t count,
                            const reshade::api::resource* resources,
                            const reshade::api::resource_usage* newStates) {
    if (!g_isEnhancedEdition || device == nullptr || resources == nullptr || newStates == nullptr ||
        enhancedExpectedWidth.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
    for (uint32_t index = 0; index < count; ++index) {
        if (resources[index].handle == enhancedVideoEncodeResource.handle) {
            enhancedVideoEncodeState = newStates[index];
        }
        if (device == enhancedCleanCandidateDevice &&
            resources[index].handle == enhancedCleanCandidateResource.handle) {
            enhancedCleanCandidateState = newStates[index];
        }
        if (device == enhancedDepthCandidateDevice &&
            resources[index].handle == enhancedDepthCandidateResource.handle) {
            enhancedDepthCandidateState = newStates[index];
        }
    }
}

void ever::OnReshadeCopyResource(reshade::api::device* device,
                                 reshade::api::resource source,
                                 reshade::api::resource destination,
                                 const char* operation) {
    if (!g_isEnhancedEdition || device == nullptr || source.handle == 0 || destination.handle == 0 ||
        enhancedExpectedWidth.load(std::memory_order_relaxed) == 0) return;

    bool destinationIsBackBuffer = false;
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
        destinationIsBackBuffer = enhancedBackBufferHandles.contains(destination.handle);
        if (destinationIsBackBuffer)
            shouldLog = enhancedLoggedCopySourceHandles.insert(source.handle).second;
    }
    if (!destinationIsBackBuffer || !shouldLog) return;

    const reshade::api::resource_desc sourceDesc = device->get_resource_desc(source);
    const reshade::api::resource_desc destinationDesc = device->get_resource_desc(destination);
    const std::string sourceName = getD3D12ResourceDebugName(device, source);
    LOG(LL_NFO, "Enhanced copy into swap-chain candidate. operation=", operation,
        " source=", Logger::hex(source.handle, 16),
        " sourceResolution=", sourceDesc.texture.width, "x", sourceDesc.texture.height,
        " sourceFormat=", static_cast<uint32_t>(sourceDesc.texture.format),
        " sourceName=", sourceName.empty() ? "<unnamed>" : sourceName,
        " destination=", Logger::hex(destination.handle, 16),
        " destinationResolution=", destinationDesc.texture.width, "x", destinationDesc.texture.height);
}

void ever::OnReshadeDestroyResource(reshade::api::resource resource) {
    if (!g_isEnhancedEdition || resource.handle == 0) return;
    {
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        if (resource.handle == enhancedVideoEncodeResource.handle) {
            LOG(LL_DBG, "Enhanced VideoEncode resource destroyed");
            enhancedVideoEncodeResource = {};
            enhancedVideoEncodeState = reshade::api::resource_usage::undefined;
            enhancedVideoResourceTracked.store(false, std::memory_order_release);
        }
        if (resource.handle == enhancedCleanCandidateResource.handle) {
            LOG(LL_DBG, "Enhanced clean color candidate destroyed");
            enhancedCleanCandidateDevice = nullptr;
            enhancedCleanCandidateResource = {};
            enhancedCleanCandidateState = reshade::api::resource_usage::undefined;
        }
        if (resource.handle == enhancedDepthCandidateResource.handle) {
            LOG(LL_DBG, "Enhanced depth candidate destroyed");
            enhancedDepthCandidateDevice = nullptr;
            enhancedDepthCandidateResource = {};
            enhancedDepthCandidateState = reshade::api::resource_usage::undefined;
        }
    }
    {
        std::lock_guard<std::mutex> diagnosticLock(enhancedDiagnosticMutex);
        enhancedLoggedRenderTargetHandles.erase(resource.handle);
        enhancedLoggedDepthStencilHandles.erase(resource.handle);
        enhancedLoggedCopySourceHandles.erase(resource.handle);
    }
}

void ever::OnReshadeBeginEffects(reshade::api::effect_runtime* runtime,
                                 reshade::api::command_list* commandList,
                                 reshade::api::resource_view targetView) {
    enhancedPostEffectsCaptureReady.store(false, std::memory_order_release);
    if (!g_isEnhancedEdition || runtime == nullptr || commandList == nullptr || targetView.handle == 0 ||
        enhancedExpectedWidth.load(std::memory_order_relaxed) == 0) return;

    std::lock_guard<std::mutex> sessionLock(mxSession);
    if (encodingSession == nullptr || !encodingSession->isCapturing) return;

    reshade::api::device* const device = commandList->get_device();
    if (device == nullptr || device != runtime->get_device() ||
        device->get_api() != reshade::api::device_api::d3d12) return;

    const reshade::api::resource target = device->get_resource_from_view(targetView);
    if (target.handle == 0) return;

    reshade::api::resource cleanSource{};
    reshade::api::resource_usage cleanState = reshade::api::resource_usage::undefined;
    {
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        if (enhancedCleanCandidateDevice != device || enhancedCleanCandidateResource.handle == 0) return;
        cleanSource = enhancedCleanCandidateResource;
        cleanState = enhancedCleanCandidateState;
    }
    if (cleanSource.handle == target.handle) return;

    const reshade::api::resource_desc sourceDesc = device->get_resource_desc(cleanSource);
    const reshade::api::resource_desc targetDesc = device->get_resource_desc(target);
    if (sourceDesc.texture.width != targetDesc.texture.width ||
        sourceDesc.texture.height != targetDesc.texture.height ||
        reshade::api::format_to_typeless(sourceDesc.texture.format) !=
            reshade::api::format_to_typeless(targetDesc.texture.format)) {
        return;
    }

    if (cleanState == reshade::api::resource_usage::undefined)
        cleanState = reshade::api::resource_usage::render_target;
    if (cleanState != reshade::api::resource_usage::copy_source)
        commandList->barrier(cleanSource, cleanState, reshade::api::resource_usage::copy_source);
    commandList->barrier(target, reshade::api::resource_usage::render_target,
                         reshade::api::resource_usage::copy_dest);
    commandList->copy_texture_region(cleanSource, 0, nullptr, target, 0, nullptr);
    commandList->barrier(target, reshade::api::resource_usage::copy_dest,
                         reshade::api::resource_usage::render_target);
    if (cleanState != reshade::api::resource_usage::copy_source)
        commandList->barrier(cleanSource, reshade::api::resource_usage::copy_source, cleanState);

    enhancedPostEffectsBackBufferHandle.store(target.handle, std::memory_order_release);
    enhancedPostEffectsCaptureReady.store(true, std::memory_order_release);
    if (!hasLoggedEnhancedPreEffectsReplacement.exchange(true)) {
        LOG(LL_NFO, "Enhanced clean pre-UI image copied into the ReShade effect target. source=",
            Logger::hex(cleanSource.handle, 16), " target=", Logger::hex(target.handle, 16),
            " resolution=", sourceDesc.texture.width, "x", sourceDesc.texture.height,
            " effectsEnabled=", runtime->get_effects_state());
    }
}

void ever::OnReshadePresent(reshade::api::effect_runtime* runtime) {
    if (!g_isEnhancedEdition || runtime == nullptr) return;

    if (enhancedExpectedWidth.load(std::memory_order_relaxed) == 0) {
        uint32_t currentRuntimeWidth = 0;
        uint32_t currentRuntimeHeight = 0;
        runtime->get_screenshot_width_and_height(&currentRuntimeWidth, &currentRuntimeHeight);
        const uint64_t currentArea = static_cast<uint64_t>(enhancedRuntimeWidth.load(std::memory_order_relaxed)) *
                                     enhancedRuntimeHeight.load(std::memory_order_relaxed);
        const uint64_t candidateArea = static_cast<uint64_t>(currentRuntimeWidth) * currentRuntimeHeight;
        if (currentRuntimeWidth != 0 && currentRuntimeHeight != 0 && candidateArea >= currentArea) {
            enhancedRuntimeWidth.store(currentRuntimeWidth, std::memory_order_relaxed);
            enhancedRuntimeHeight.store(currentRuntimeHeight, std::memory_order_relaxed);
            enhancedRuntimeSource.store(runtime, std::memory_order_relaxed);
        }
    }

    std::lock_guard<std::mutex> captureLock(reshadeCaptureMutex);
    std::lock_guard<std::mutex> sessionLock(mxSession);
    if (encodingSession == nullptr || !encodingSession->isCapturing) return;

    const uint32_t expectedWidth = encodingSession->width();
    const uint32_t expectedHeight = encodingSession->height();
    reshade::api::device* const device = runtime->get_device();
    reshade::api::resource source = runtime->get_current_back_buffer();
    if (source.handle == 0 || device == nullptr) return;
    reshade::api::resource_desc sourceDesc = device->get_resource_desc(source);
    reshade::api::resource_usage sourceState = reshade::api::resource_usage::present;
    bool usesCleanCandidate = false;
    const bool usesPostEffectsCleanBackBuffer =
        enhancedPostEffectsCaptureReady.exchange(false, std::memory_order_acq_rel) &&
        enhancedPostEffectsBackBufferHandle.load(std::memory_order_acquire) == source.handle;
    const uint32_t width = sourceDesc.texture.width;
    const uint32_t height = sourceDesc.texture.height;
    if (!hasLoggedEnhancedBackBuffer.exchange(true)) {
        const std::string name = getD3D12ResourceDebugName(device, source);
        LOG(LL_NFO, "Enhanced active swap-chain back buffer. handle=", Logger::hex(source.handle, 16),
            " resolution=", width, "x", height,
            " format=", static_cast<uint32_t>(sourceDesc.texture.format),
            " name=", name.empty() ? "<unnamed>" : name);
    }

    if (!usesPostEffectsCleanBackBuffer) {
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        if (enhancedCleanCandidateDevice == device && enhancedCleanCandidateResource.handle != 0) {
            const reshade::api::resource_desc candidateDesc =
                device->get_resource_desc(enhancedCleanCandidateResource);
            const reshade::api::format candidateFormat =
                reshade::api::format_to_default_typed(candidateDesc.texture.format, 0);
            const bool supportedColorFormat =
                candidateFormat == reshade::api::format::r8g8b8a8_unorm ||
                candidateFormat == reshade::api::format::r8g8b8x8_unorm ||
                candidateFormat == reshade::api::format::b8g8r8a8_unorm ||
                candidateFormat == reshade::api::format::b8g8r8x8_unorm;
            if (candidateDesc.texture.width == expectedWidth &&
                candidateDesc.texture.height == expectedHeight && supportedColorFormat) {
                source = enhancedCleanCandidateResource;
                sourceDesc = candidateDesc;
                sourceState = enhancedCleanCandidateState == reshade::api::resource_usage::undefined
                    ? reshade::api::resource_usage::render_target
                    : enhancedCleanCandidateState;
                usesCleanCandidate = true;
            }
        }
    }

    if ((usesCleanCandidate || usesPostEffectsCleanBackBuffer) &&
        !hasLoggedEnhancedCleanCapture.exchange(true)) {
        const std::string name = getD3D12ResourceDebugName(device, source);
        LOG(LL_NFO, "Enhanced clean capture selected. source=",
            usesPostEffectsCleanBackBuffer ? "post-ReShade-clean-backbuffer" : "pre-UI-render-target",
            " handle=", Logger::hex(source.handle, 16),
            " resolution=", sourceDesc.texture.width, "x", sourceDesc.texture.height,
            " format=", static_cast<uint32_t>(sourceDesc.texture.format),
            " state=", static_cast<uint32_t>(sourceState),
            " name=", name.empty() ? "<unnamed>" : name);
    }

    if (sourceDesc.texture.width != expectedWidth || sourceDesc.texture.height != expectedHeight) {
        if (!hasLoggedEnhancedCaptureResolutionMismatch.exchange(true)) {
            LOG(LL_WRN, "Enhanced capture source resolution does not match the EVER output. source=",
                sourceDesc.texture.width, "x", sourceDesc.texture.height,
                " export=", expectedWidth, "x", expectedHeight,
                ". Refusing to scale because EVER must preserve the configured high-resolution export.");
        }
        return;
    }

    const size_t rowPitch = static_cast<size_t>(expectedWidth) * 4;
    if (rowPitch == 0 || rowPitch > std::numeric_limits<uint32_t>::max()) {
        LOG(LL_ERR, "Enhanced ReShade capture produced invalid dimensions: ", expectedWidth, "x", expectedHeight);
        return;
    }

    reshade::api::resource depthSource{};
    reshade::api::resource_usage depthState = reshade::api::resource_usage::undefined;
    reshade::api::resource_desc depthDesc{};
    const reshade::api::resource_desc* depthDescPtr = nullptr;
    if (Config::Manager::export_openexr) {
        std::lock_guard<std::mutex> resourceLock(enhancedResourceMutex);
        if (enhancedDepthCandidateDevice == device && enhancedDepthCandidateResource.handle != 0) {
            depthSource = enhancedDepthCandidateResource;
            depthState = enhancedDepthCandidateState == reshade::api::resource_usage::undefined
                ? reshade::api::resource_usage::depth_stencil_write
                : enhancedDepthCandidateState;
            depthDesc = device->get_resource_desc(depthSource);
            depthDescPtr = &depthDesc;
        }
    }

    std::lock_guard<std::mutex> readbackLock(enhancedReadbackMutex);
    if (!ensureEnhancedReadbackResourcesLocked(runtime, sourceDesc, depthDescPtr)) return;
    const bool gpuMotionBlur = isCustomFrameRateSupported &&
        Config::Manager::motion_blur_samples != 0 &&
        !Config::Manager::export_openexr &&
        ensureEnhancedGpuMotionBlurResourcesLocked(runtime, sourceDesc);

    const uint64_t activeGeneration = enhancedCaptureGeneration.load(std::memory_order_acquire);
    uint64_t completedGeneration = 0;
    bool gpuMotionBlurOutput = false;
    if (consumeEnhancedReadbackLocked(
            reshadeCapturePixels, enhancedCaptureDepth, completedGeneration,
            gpuMotionBlurOutput) &&
        completedGeneration == activeGeneration &&
        enhancedPendingVideoFrames.load(std::memory_order_acquire) != 0) {
        const HRESULT result = gpuMotionBlurOutput
            ? processEnhancedGpuMotionBlurOutput(
                reshadeCapturePixels.data(), static_cast<uint32_t>(rowPitch), expectedHeight)
            : processEnhancedCapturedVideoSample(
                reshadeCapturePixels.data(), static_cast<uint32_t>(rowPitch), expectedHeight,
                enhancedCaptureDepth.empty() ? nullptr : enhancedCaptureDepth.data(),
                enhancedCaptureDepth.empty() ? 0u :
                    expectedWidth * static_cast<uint32_t>(sizeof(float)));
        if (FAILED(result)) {
            LOG(LL_ERR, "Failed to enqueue Enhanced frame. hr=",
                Logger::hex(static_cast<uint32_t>(result), 8));
            return;
        }
        if (!hasLoggedEnhancedCaptureSuccess.exchange(true)) {
            LOG(LL_NFO, "Enhanced frame capture active via asynchronous ReShade D3D12 readback. source=",
                usesPostEffectsCleanBackBuffer ? "post-ReShade-clean-backbuffer" :
                    (usesCleanCandidate ? "clean-pre-UI-render-target" : "swap-chain"),
                " api=", static_cast<uint32_t>(device->get_api()),
                " resolution=", expectedWidth, "x", expectedHeight,
                " gpuMotionBlur=", gpuMotionBlurOutput);
        }
    }

    const uint32_t pendingFrames = enhancedPendingVideoFrames.load(std::memory_order_acquire);
    if (pendingFrames > enhancedReadbackOrder.size()) {
        if (gpuMotionBlur) {
            processEnhancedGpuMotionBlurSubframeLocked(
                runtime, source, sourceState, activeGeneration);
        } else {
            queueEnhancedReadbackLocked(
                runtime, source, sourceState, depthSource, depthState, activeGeneration);
        }
    }
}

HRESULT ExportHooks::D3D11CreateDeviceAndSwapChain::Implementation(
    IDXGIAdapter* p_adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* p_feature_levels, UINT feature_levels, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* p_swap_chain_desc, IDXGISwapChain** pp_swap_chain, ID3D11Device** pp_device,
    D3D_FEATURE_LEVEL* p_feature_level, ID3D11DeviceContext** pp_immediate_context) {
    PRE();
    const HRESULT result =
        OriginalFunc(p_adapter, driver_type, software, flags, p_feature_levels, feature_levels, sdk_version,
                     p_swap_chain_desc, pp_swap_chain, pp_device, p_feature_level, pp_immediate_context);
    std::string swapChainPtrString = "<null>";
    if (pp_swap_chain && *pp_swap_chain) {
        swapChainPtrString = Logger::hex(reinterpret_cast<uint64_t>(*pp_swap_chain), 16);
    }
    LOG(LL_DBG, "D3D11CreateDeviceAndSwapChain returned ", Logger::hex(result, 8), "; swap chain ptr:",
        swapChainPtrString);

    if (SUCCEEDED(result) && pp_swap_chain && *pp_swap_chain) {
            captureAndHookSwapChain(*pp_swap_chain, "IDXGISwapChain::Present");
        installFactoryHooksFromSwapChain(*pp_swap_chain, "D3D11CreateDeviceAndSwapChain");
    }

    if (SUCCEEDED(result) && pp_immediate_context && *pp_immediate_context) {
        try {
            PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, Draw, *pp_immediate_context);
            PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, DrawIndexed, *pp_immediate_context);
            PERFORM_MEMBER_HOOK_REQUIRED(ID3D11DeviceContext, OMSetRenderTargets, *pp_immediate_context);
        } catch (...) {
            LOG(LL_ERR, "Hooking ID3D11DeviceContext functions failed");
        }
    }

    if (SUCCEEDED(result) && pp_device && *pp_device && pp_immediate_context && *pp_immediate_context) {
        try {
            ComPtr<IDXGIDevice> pDXGIDevice;
            REQUIRE((*pp_device)->QueryInterface(pDXGIDevice.GetAddressOf()),
                    "Failed to get IDXGIDevice from ID3D11Device");

            ComPtr<IDXGIAdapter> pDXGIAdapter;
            REQUIRE(
                pDXGIDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(pDXGIAdapter.GetAddressOf())),
                "Failed to get IDXGIAdapter");

            ComPtr<IDXGIFactory> localFactory;
            REQUIRE(pDXGIAdapter->GetParent(__uuidof(IDXGIFactory),
                                            reinterpret_cast<void**>(localFactory.GetAddressOf())),
                    "Failed to get IDXGIFactory");
            installFactoryHooks(localFactory.Get());

            ever::prepareDeferredContext(*pp_device, *pp_immediate_context);
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        } catch (...) {
            LOG(LL_ERR, "Hooking IDXGISwapChain support structures failed");
        }
    }
    POST();
    return result;
}

HRESULT IDXGIFactoryHooks::CreateSwapChain::Implementation(IDXGIFactory* pThis, IUnknown* pDevice,
                                                           DXGI_SWAP_CHAIN_DESC* pDesc,
                                                           IDXGISwapChain** ppSwapChain) {
    PRE();
    const HRESULT hr = OriginalFunc(pThis, pDevice, pDesc, ppSwapChain);
    std::string swapChainPtr = "<null>";
    if (ppSwapChain && *ppSwapChain) {
        swapChainPtr = Logger::hex(reinterpret_cast<uint64_t>(*ppSwapChain), 16);
    }
    LOG(LL_DBG, "IDXGIFactory::CreateSwapChain hr:", Logger::hex(hr, 8), " swap chain:", swapChainPtr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        captureAndHookSwapChain(*ppSwapChain, "IDXGIFactory::CreateSwapChain");
    }

    POST();
    return hr;
}

HRESULT IDXGIFactory2Hooks::CreateSwapChainForHwnd::Implementation(
    IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    PRE();
    const HRESULT hr = OriginalFunc(pThis, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    std::string swapChainPtr = "<null>";
    if (ppSwapChain && *ppSwapChain) {
        swapChainPtr = Logger::hex(reinterpret_cast<uint64_t>(*ppSwapChain), 16);
    }
    LOG(LL_DBG, "IDXGIFactory2::CreateSwapChainForHwnd hr:", Logger::hex(hr, 8), " swap chain:", swapChainPtr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        captureSwapChainFromSwapChain1(*ppSwapChain, "IDXGIFactory2::CreateSwapChainForHwnd");
    }

    POST();
    return hr;
}

HRESULT IDXGIFactory2Hooks::CreateSwapChainForCoreWindow::Implementation(
    IDXGIFactory2* pThis, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    PRE();
    const HRESULT hr = OriginalFunc(pThis, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    std::string swapChainPtr = "<null>";
    if (ppSwapChain && *ppSwapChain) {
        swapChainPtr = Logger::hex(reinterpret_cast<uint64_t>(*ppSwapChain), 16);
    }
    LOG(LL_DBG, "IDXGIFactory2::CreateSwapChainForCoreWindow hr:", Logger::hex(hr, 8), " swap chain:", swapChainPtr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        captureSwapChainFromSwapChain1(*ppSwapChain, "IDXGIFactory2::CreateSwapChainForCoreWindow");
    }

    POST();
    return hr;
}

HRESULT IDXGIFactory2Hooks::CreateSwapChainForComposition::Implementation(
    IDXGIFactory2* pThis, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    PRE();
    const HRESULT hr = OriginalFunc(pThis, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    std::string swapChainPtr = "<null>";
    if (ppSwapChain && *ppSwapChain) {
        swapChainPtr = Logger::hex(reinterpret_cast<uint64_t>(*ppSwapChain), 16);
    }
    LOG(LL_DBG, "IDXGIFactory2::CreateSwapChainForComposition hr:", Logger::hex(hr, 8),
        " swap chain:", swapChainPtr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        captureSwapChainFromSwapChain1(*ppSwapChain, "IDXGIFactory2::CreateSwapChainForComposition");
    }

    POST();
    return hr;
}

HRESULT IDXGISwapChainHooks::Present::Implementation(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags) {
    if (!mainSwapChain) {
        if (Flags & DXGI_PRESENT_TEST) {
            LOG(LL_TRC, "DXGI_PRESENT_TEST!");
        } else {
            ever::OnPresent(pThis);
        }
    }

    return OriginalFunc(pThis, SyncInterval, Flags);
}

void ever::initialize() {
    PRE();

    try {
        LOG(LL_NFO, "Initializing Media Foundation hook");
        PERFORM_NAMED_IMPORT_HOOK_REQUIRED("mfreadwrite.dll", MFCreateSinkWriterFromURL);

        if (g_isEnhancedEdition) {
            LOG(LL_NFO, "Detected GTA V Enhanced");
            LOG(LL_NFO, "Enhanced compatibility mode enabled: Media Foundation audio/encoding hooks and ",
                "ReShade cross-API frame capture are active");
            LOG(LL_NFO, "Scanning the isolated Enhanced render-time wrapper for custom FPS support");

            patternScanner.reset(new ever::hooking::PatternScanner());
            patternScanner->initialize();
            uint64_t pEnhancedGetRenderTimeBase = 0;
            uint64_t pEnhancedStartBakeProject = 0;
            uint64_t pEnhancedCleanupReplayPlayback = 0;
            uint64_t pEnhancedIsPendingBakeStart = 0;
            uint64_t pEnhancedKillPlaybackOrBake = 0;
            patternScanner->addPattern(
                "enhanced_get_render_time_base_function",
                ever::hooking::patterns::getRenderTimeBaseEnhanced,
                &pEnhancedGetRenderTimeBase);
            patternScanner->addPattern(
                "enhanced_start_bake_project",
                ever::hooking::patterns::startBakeProjectEnhanced,
                &pEnhancedStartBakeProject);
            patternScanner->addPattern(
                "enhanced_cleanup_replay_playback",
                ever::hooking::patterns::cleanupReplayPlaybackEnhanced,
                &pEnhancedCleanupReplayPlayback);
            patternScanner->addPattern(
                "enhanced_is_pending_bake_start",
                ever::hooking::patterns::isPendingBakeStartEnhanced,
                &pEnhancedIsPendingBakeStart);
            patternScanner->addPattern(
                "enhanced_kill_playback_or_bake",
                ever::hooking::patterns::killPlaybackOrBakeEnhanced,
                &pEnhancedKillPlaybackOrBake);
            patternScanner->performScan();
            logResolvedHookTarget("EnhancedGetRenderTimeBase", pEnhancedGetRenderTimeBase);
            logResolvedHookTarget("EnhancedStartBakeProject", pEnhancedStartBakeProject);
            logResolvedHookTarget("EnhancedCleanupReplayPlayback", pEnhancedCleanupReplayPlayback);
            logResolvedHookTarget("EnhancedIsPendingBakeStart", pEnhancedIsPendingBakeStart);
            logResolvedHookTarget("EnhancedKillPlaybackOrBake", pEnhancedKillPlaybackOrBake);

            const bool expectedEnhancedEntry = pEnhancedGetRenderTimeBase != 0 && memoryStartsWith(
                pEnhancedGetRenderTimeBase,
                {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xFF,
                 0x50, 0x18, 0x89, 0xC1, 0x48, 0x83, 0xC4, 0x28});
            if (expectedEnhancedEntry && installAbsoluteJumpHook(
                    pEnhancedGetRenderTimeBase,
                    reinterpret_cast<uintptr_t>(GameHooks::GetRenderTimeBase::Implementation))) {
                isCustomFrameRateSupported = true;
                LOG(LL_NFO, "Enhanced custom FPS hook installed successfully");
            } else if (pEnhancedGetRenderTimeBase == 0) {
                LOG(LL_WRN, "Enhanced custom FPS wrapper was not found; custom FPS remains disabled for this game build");
            } else if (!expectedEnhancedEntry) {
                LOG(LL_WRN, "Enhanced custom FPS candidate failed entry validation; hook was not installed");
            } else {
                LOG(LL_ERR, "Failed to install the validated Enhanced custom FPS hook");
            }

            bool enhancedDualPassHooksInstalled = false;
            if (pEnhancedStartBakeProject && pEnhancedCleanupReplayPlayback &&
                pEnhancedIsPendingBakeStart && pEnhancedKillPlaybackOrBake) {
                try {
                    PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(
                        EnhancedStartBakeProject, pEnhancedStartBakeProject,
                        PLH::x64Detour::detour_scheme_t::INPLACE);
                    pCleanupReplayPlaybackOriginal = reinterpret_cast<CleanupReplayPlaybackInternalFunc>(
                        pEnhancedCleanupReplayPlayback);
                    std::memcpy(g_cleanupOriginalBytes,
                                reinterpret_cast<const void*>(pEnhancedCleanupReplayPlayback),
                                sizeof(g_cleanupOriginalBytes));
                    if (!installAbsoluteJumpHook(
                            pEnhancedCleanupReplayPlayback,
                            reinterpret_cast<uintptr_t>(
                                GameHooks::EnhancedCleanupReplayPlayback::Implementation))) {
                        throw std::runtime_error("Failed to hook EnhancedCleanupReplayPlayback manually");
                    }
                    LOG(LL_NFO, "Enhanced CleanupReplayPlayback manual hook installed. target=",
                        reinterpret_cast<void*>(pEnhancedCleanupReplayPlayback));
                    pEnhancedKillPlaybackOrBakeOriginal =
                        reinterpret_cast<EnhancedKillPlaybackOrBakeFunc>(
                            pEnhancedKillPlaybackOrBake);
                    std::memcpy(g_enhancedKillPlaybackOrBakeOriginalBytes,
                                reinterpret_cast<const void*>(pEnhancedKillPlaybackOrBake),
                                sizeof(g_enhancedKillPlaybackOrBakeOriginalBytes));
                    if (!prepareEnhancedKillPass1ExitPatch(pEnhancedKillPlaybackOrBake)) {
                        throw std::runtime_error(
                            "Failed to resolve EnhancedKillPlaybackOrBake pass-one exit boundary");
                    }
                    if (!installAbsoluteJumpHook(
                            pEnhancedKillPlaybackOrBake,
                            reinterpret_cast<uintptr_t>(
                                GameHooks::EnhancedKillPlaybackOrBake::Implementation))) {
                        throw std::runtime_error("Failed to hook EnhancedKillPlaybackOrBake manually");
                    }
                    LOG(LL_NFO, "Enhanced KillPlaybackOrBake manual hook installed. target=",
                        reinterpret_cast<void*>(pEnhancedKillPlaybackOrBake));
                    g_isPendingBakeStartAddress = pEnhancedIsPendingBakeStart;
                    g_isPendingBakeStartStatePtr =
                        resolveIsPendingBakeStartStatePointer(pEnhancedIsPendingBakeStart);
                    if (!g_isPendingBakeStartStatePtr || !installAbsoluteJumpHook(
                            pEnhancedIsPendingBakeStart,
                            reinterpret_cast<uintptr_t>(&IsPendingBakeStart_Hook))) {
                        throw std::runtime_error("Failed to hook Enhanced IsPendingBakeStart");
                    }
                    LOG(LL_NFO, "Enhanced IsPendingBakeStart manual hook installed. state=",
                        reinterpret_cast<void*>(g_isPendingBakeStartStatePtr));
                    enhancedDualPassHooksInstalled = true;
                    isEnhancedDualPassSupported = true;
                    LOG(LL_NFO, "Enhanced dual-pass state-machine hooks installed successfully");
                } catch (const std::exception& ex) {
                    LOG(LL_ERR, "Enhanced dual-pass hook installation failed: ", ex.what());
                } catch (...) {
                    LOG(LL_ERR, "Enhanced dual-pass hook installation failed with an unknown exception");
                }
            } else {
                LOG(LL_WRN, "One or more Enhanced dual-pass state-machine entry points were not found");
            }

            LOG(LL_NFO, "Skipping Legacy-only LoadLibrary and diagnostic IAT hooks in Enhanced");
            LOG(enhancedDualPassHooksInstalled ? LL_NFO : LL_WRN,
                "Enhanced dual-pass audio workflow is ",
                enhancedDualPassHooksInstalled ? "available for runtime testing" : "unavailable for this game build");
            LOG(LL_NFO, "Enhanced feature path active: clean pre-UI capture, custom FPS, multi-sample motion blur, ",
                "dual-pass audio, and OpenEXR/depth export are available");
            POST();
            return;
        }
        LOG(LL_NFO, "Initializing LoadLibrary hook");
        PERFORM_NAMED_IMPORT_HOOK_REQUIRED("kernel32.dll", LoadLibraryA);
        PERFORM_NAMED_IMPORT_HOOK_REQUIRED("kernel32.dll", LoadLibraryW);

        LOG(LL_NFO, "Hooking SetUnhandledExceptionFilter (will log GTA V's UEF replacement)");
        if (FAILED(::ever::hooking::hookNamedImport(
                "kernel32.dll", "SetUnhandledExceptionFilter",
                ImportHooks::SetUnhandledExceptionFilter::Implementation,
                &ImportHooks::SetUnhandledExceptionFilter::OriginalFunc,
                ImportHooks::SetUnhandledExceptionFilter::Hook))) {
            LOG(LL_WRN, "SetUnhandledExceptionFilter IAT hook failed (non-fatal - VEH still active)");
        } else {
            LOG(LL_NFO, "SetUnhandledExceptionFilter IAT hook installed");
        }

        LOG(LL_NFO, "Hooking MessageBoxW (will capture GTA V fatal error dialogs)");
        if (FAILED(::ever::hooking::hookNamedImport(
                "user32.dll", "MessageBoxW",
                ImportHooks::MessageBoxW::Implementation,
                &ImportHooks::MessageBoxW::OriginalFunc,
                ImportHooks::MessageBoxW::Hook))) {
            LOG(LL_WRN, "MessageBoxW IAT hook failed (non-fatal - RAGE error messages won't be pre-captured)");
        } else {
            LOG(LL_NFO, "MessageBoxW IAT hook installed - GTA V fatal error dialogs will be captured");
        }


        patternScanner.reset(new ever::hooking::PatternScanner());
        patternScanner->initialize();

        MODULEINFO info;
        GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &info, sizeof(info));
        LOG(LL_NFO, "Image base:", ((void*)info.lpBaseOfDll));

        uint64_t pGetRenderTimeBase = NULL;
        uint64_t pCreateTexture = NULL;
        uint64_t pCreateThread = NULL;
        uint64_t pStartBakeProject = NULL;
        uint64_t pWatermarkRenderer = NULL;
        uint64_t pCleanupReplayPlayback = NULL;
        uint64_t pIsPendingBakeStart = NULL;
        uint64_t pKillPlaybackOrBake = NULL;
        uint64_t pSetUserConfirmationScreen = NULL;
        uint64_t pHasVideoRenderErrored = NULL;
        uint64_t pShouldShowLoadingScreen = NULL;
        uint64_t pPuddlesRipplesUpdate = NULL;
        uint64_t pClothManagerUpdate = NULL;
        
        patternScanner->addPattern("get_render_time_base_function", 
                                    ever::hooking::patterns::getRenderTimeBase, 
                                    &pGetRenderTimeBase);
        patternScanner->addPattern("create_thread_function", 
                                    ever::hooking::patterns::createThread, 
                                    &pCreateThread);
        patternScanner->addPattern("create_texture_function", 
                                    ever::hooking::patterns::createTexture, 
                                    &pCreateTexture);
        patternScanner->addPattern("start_bake_project", 
                                    ever::hooking::patterns::startBakeProject, 
                                    &pStartBakeProject);
        patternScanner->addPattern("watermark_renderer_render", 
                                    ever::hooking::patterns::watermarkRendererRender, 
                                    &pWatermarkRenderer);
        patternScanner->addPattern("cleanup_replay_playback_internal", 
                                    ever::hooking::patterns::cleanupReplayPlayback, 
                                    &pCleanupReplayPlayback);
        patternScanner->addPattern("is_pending_bake_start", 
                                    ever::hooking::patterns::isPendingBakeStart, 
                                    &pIsPendingBakeStart);
        patternScanner->addPattern("kill_playback_or_bake", 
                                    ever::hooking::patterns::killPlaybackOrBake, 
                                    &pKillPlaybackOrBake);
        patternScanner->addPattern("set_user_confirmation_screen",
                                    ever::hooking::patterns::setUserConfirmationScreen,
                                    &pSetUserConfirmationScreen);
        patternScanner->addPattern("has_video_render_errored",
                                    ever::hooking::patterns::hasVideoRenderErrored,
                                    &pHasVideoRenderErrored);
        patternScanner->addPattern("should_show_loading_screen",
                                    ever::hooking::patterns::shouldShowLoadingScreen,
                                    &pShouldShowLoadingScreen);
        patternScanner->addPattern("puddles_ripples_update",
                                    ever::hooking::patterns::puddlesRipplesUpdate,
                                    &pPuddlesRipplesUpdate);
        patternScanner->addPattern("cloth_manager_update",
                                    ever::hooking::patterns::clothManagerUpdate,
                                    &pClothManagerUpdate);

        patternScanner->performScan();

        if (pPuddlesRipplesUpdate) {
            const uint64_t resolvedPuddlesRipplesUpdate =
                resolvePuddlesRipplesUpdateFunction(pPuddlesRipplesUpdate);
            if (resolvedPuddlesRipplesUpdate != pPuddlesRipplesUpdate) {
                LOG(LL_DBG, "PuddlesRipplesUpdate anchor adjusted from ",
                    Logger::hex(pPuddlesRipplesUpdate, 16), " to ",
                    Logger::hex(resolvedPuddlesRipplesUpdate, 16));
            }
            pPuddlesRipplesUpdate = resolvedPuddlesRipplesUpdate;
        }

        logResolvedHookTarget("GetRenderTimeBase", pGetRenderTimeBase);
        logResolvedHookTarget("CreateTexture", pCreateTexture);
        logResolvedHookTarget("CreateThread", pCreateThread);
        logResolvedHookTarget("StartBakeProject", pStartBakeProject);
        logResolvedHookTarget("WatermarkRenderer", pWatermarkRenderer);
        logResolvedHookTarget("CleanupReplayPlaybackInternal", pCleanupReplayPlayback);
        logResolvedHookTarget("IsPendingBakeStart", pIsPendingBakeStart);
        logResolvedHookTarget("KillPlaybackOrBake", pKillPlaybackOrBake);
        logResolvedHookTarget("SetUserConfirmationScreen", pSetUserConfirmationScreen);
        logResolvedHookTarget("HasVideoRenderErrored", pHasVideoRenderErrored);
        logResolvedHookTarget("ShouldShowLoadingScreen", pShouldShowLoadingScreen);
        logResolvedHookTarget("PuddlesRipplesUpdate", pPuddlesRipplesUpdate);
        logResolvedHookTarget("ClothManagerUpdate", pClothManagerUpdate);

        try {
            if (pGetRenderTimeBase) {
                const auto hookAddress = reinterpret_cast<uintptr_t>(GameHooks::GetRenderTimeBase::Implementation);
                if (installAbsoluteJumpHook(pGetRenderTimeBase, hookAddress)) {
                    isCustomFrameRateSupported = true;
                } else {
                    LOG(LL_ERR, "Failed to install GetRenderTimeBase hook");
                }
            } else {
                LOG(LL_ERR, "Could not find the address for FPS function.");
                LOG(LL_ERR, "Custom FPS support is DISABLED!!!");
            }

            if (pCreateTexture) {
                PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(CreateTexture, pCreateTexture,
                                                      PLH::x64Detour::detour_scheme_t::INPLACE);
                LOG(LL_DBG, "CreateTexture hook installed. trampoline:",
                    reinterpret_cast<void*>(GameHooks::CreateTexture::OriginalFunc));
            } else {
                LOG(LL_ERR, "Could not find the address for CreateTexture function.");
            }

            if (pCreateThread) {
                PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(CreateThread, pCreateThread,
                                                      PLH::x64Detour::detour_scheme_t::INPLACE);
                LOG(LL_DBG, "CreateThread hook installed. trampoline:",
                    reinterpret_cast<void*>(GameHooks::CreateThread::OriginalFunc));
            } else {
                LOG(LL_ERR, "Could not find the address for CreateThread function.");
            }

            if (pStartBakeProject) {
                PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(StartBakeProject, pStartBakeProject,
                                                      PLH::x64Detour::detour_scheme_t::INPLACE);
                LOG(LL_NFO, "StartBakeProject hook installed. trampoline:",
                    reinterpret_cast<void*>(GameHooks::StartBakeProject::OriginalFunc));
            } else {
                LOG(LL_ERR, "Could not find the address for StartBakeProject function.");
                LOG(LL_ERR, "Dual-pass audio rendering will NOT be available!");
            }
            
            // Install manual hook for CleanupReplayPlaybackInternal
            if (pCleanupReplayPlayback) {
                pCleanupReplayPlaybackOriginal = reinterpret_cast<CleanupReplayPlaybackInternalFunc>(pCleanupReplayPlayback);
                
                // Store original bytes BEFORE installing hook
                memcpy(g_cleanupOriginalBytes, reinterpret_cast<void*>(pCleanupReplayPlayback), 12);
                LOG(LL_DBG, "Stored original bytes: ", 
                    std::hex, std::setw(2), std::setfill('0'),
                    static_cast<int>(g_cleanupOriginalBytes[0]), " ",
                    static_cast<int>(g_cleanupOriginalBytes[1]), " ",
                    static_cast<int>(g_cleanupOriginalBytes[2]), " ",
                    static_cast<int>(g_cleanupOriginalBytes[3]));
                
                // Install hook (will overwrite with jump)
                installAbsoluteJumpHook(pCleanupReplayPlayback, 
                    reinterpret_cast<uintptr_t>(&CleanupReplayPlaybackInternal_Hook));
                
                LOG(LL_NFO, "CleanupReplayPlaybackInternal manual hook installed successfully");
                LOG(LL_DBG, "  Original function: ", reinterpret_cast<void*>(pCleanupReplayPlayback));
                LOG(LL_DBG, "  Can safely unhook/rehook using stored original bytes");
            } else {
                LOG(LL_WRN, "Could not find CleanupReplayPlaybackInternal function.");
                LOG(LL_WRN, "Dual-pass rendering may experience 'no space left' errors!");
            }
            
            if (pIsPendingBakeStart) {
                const uint64_t resolvedIsPendingBakeStart = resolveIsPendingBakeStartFunction(pIsPendingBakeStart);
                if (resolvedIsPendingBakeStart != pIsPendingBakeStart) {
                    LOG(LL_DBG, "IsPendingBakeStart anchor adjusted from ",
                        Logger::hex(pIsPendingBakeStart, 16), " to ",
                        Logger::hex(resolvedIsPendingBakeStart, 16));
                }

                pIsPendingBakeStart = resolvedIsPendingBakeStart;
                g_isPendingBakeStartStatePtr = resolveIsPendingBakeStartStatePointer(pIsPendingBakeStart);
                if (g_isPendingBakeStartStatePtr) {
                    LOG(LL_DBG, "IsPendingBakeStart state pointer resolved at ",
                        reinterpret_cast<void*>(g_isPendingBakeStartStatePtr));
                } else {
                    LOG(LL_WRN, "IsPendingBakeStart: Could not resolve state pointer from function bytes");
                }
                g_isPendingBakeStartAddress = pIsPendingBakeStart;
                const auto hookAddr = reinterpret_cast<uintptr_t>(&IsPendingBakeStart_Hook);
                // Note: Function is 11 bytes but we patch 12 bytes - should be safe
                if (installAbsoluteJumpHook(pIsPendingBakeStart, hookAddr)) {
                    LOG(LL_NFO, "IsPendingBakeStart manual hook installed successfully");
                    LOG(LL_DBG, "  Original function: ", reinterpret_cast<void*>(pIsPendingBakeStart));
                    LOG(LL_DBG, "  Hook function: ", reinterpret_cast<void*>(hookAddr));
                } else {
                    LOG(LL_ERR, "Failed to install IsPendingBakeStart manual hook");
                }
            } else {
                LOG(LL_WRN, "Could not find IsPendingBakeStart function.");
            }
            
            if (pKillPlaybackOrBake) {
                pKillPlaybackOrBakeOriginal = reinterpret_cast<KillPlaybackOrBakeFunc>(pKillPlaybackOrBake);
                
                // Store original bytes BEFORE installing hook
                memcpy(g_killPlaybackOrBakeOriginalBytes, reinterpret_cast<void*>(pKillPlaybackOrBake), 12);
                LOG(LL_DBG, "KillPlaybackOrBake: Stored original bytes");
                
                // Install hook (will overwrite with jump)
                installAbsoluteJumpHook(pKillPlaybackOrBake, 
                    reinterpret_cast<uintptr_t>(&KillPlaybackOrBake_Hook));
                
                LOG(LL_NFO, "KillPlaybackOrBake manual hook installed successfully");
                LOG(LL_DBG, "  Original function: ", reinterpret_cast<void*>(pKillPlaybackOrBake));
                LOG(LL_DBG, "  This preserves ms_recordingInstanceIndex after Pass 1");
            } else {
                LOG(LL_WRN, "Could not find KillPlaybackOrBake function.");
                LOG(LL_WRN, "Dual-pass rendering may experience 'insufficient storage' errors on consecutive renders!");
            }

            if (pSetUserConfirmationScreen) {
                pSetUserConfirmationScreenOriginal = reinterpret_cast<SetUserConfirmationScreenFunc>(pSetUserConfirmationScreen);

                std::memcpy(g_setUserConfirmationScreenOriginalBytes,
                            reinterpret_cast<void*>(pSetUserConfirmationScreen),
                            12);

                installAbsoluteJumpHook(pSetUserConfirmationScreen,
                    reinterpret_cast<uintptr_t>(&SetUserConfirmationScreen_Hook));

                LOG(LL_NFO, "SetUserConfirmationScreen manual hook installed successfully");
                LOG(LL_DBG, "  Original function: ", reinterpret_cast<void*>(pSetUserConfirmationScreen));

                resolveWantDelayedCloseAddress(pSetUserConfirmationScreen);
            } else {
                LOG(LL_WRN, "Could not find SetUserConfirmationScreen function.");
                LOG(LL_WRN, "Pass 1 completion popup suppression will not work reliably.");
            }

            if (pHasVideoRenderErrored) {
                PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(HasVideoRenderErrored, pHasVideoRenderErrored,
                                                      PLH::x64Detour::detour_scheme_t::INPLACE);
                LOG(LL_NFO, "HasVideoRenderErrored hook installed. trampoline:",
                    reinterpret_cast<void*>(GameHooks::HasVideoRenderErrored::OriginalFunc));
            } else {
                LOG(LL_WRN, "Could not find HasVideoRenderErrored function.");
            }

            if (pShouldShowLoadingScreen) {
                PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(ShouldShowLoadingScreen, pShouldShowLoadingScreen,
                                                      PLH::x64Detour::detour_scheme_t::INPLACE);
                LOG(LL_NFO, "ShouldShowLoadingScreen hook installed. trampoline:",
                    reinterpret_cast<void*>(GameHooks::ShouldShowLoadingScreen::OriginalFunc));
            } else {
                LOG(LL_WRN, "Could not find ShouldShowLoadingScreen function.");
            }

            if (pPuddlesRipplesUpdate) {
                const bool oldEntry = memoryStartsWith(
                    pPuddlesRipplesUpdate,
                    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9});
                const bool b3751Entry = memoryStartsWith(
                    pPuddlesRipplesUpdate,
                    {0x48, 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05});
                const bool b3751AltEntry = memoryStartsWith(
                    pPuddlesRipplesUpdate,
                    {0x53, 0x48, 0x83, 0xEC, 0x60, 0x8A, 0x05});

                if (!oldEntry && !b3751Entry && !b3751AltEntry) {
                    LOG(LL_WRN, "Skipping PuddlesRipplesUpdate hook: resolved address is not function entry");
                } else {
                    PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(PuddlesRipplesUpdate, pPuddlesRipplesUpdate,
                                                            PLH::x64Detour::detour_scheme_t::INPLACE);
                    LOG(LL_NFO, "PuddlesRipplesUpdate hook installed. trampoline:",
                        reinterpret_cast<void*>(GameHooks::PuddlesRipplesUpdate::OriginalFunc));
                }
            } else {
                LOG(LL_WRN, "Could not find PuddlesRipplesUpdate function.");
            }

            if (pClothManagerUpdate) {
                const bool isExpectedEntry = memoryStartsWith(
                    pClothManagerUpdate,
                    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10});

                if (!isExpectedEntry) {
                    LOG(LL_WRN, "Skipping ClothManagerUpdate hook: resolved address does not match expected prologue");
                    LOG(LL_WRN, "Flag/cloth wind simulation will NOT be corrected during video export.");
                } else {
                    const uint8_t* pInstr = reinterpret_cast<const uint8_t*>(pClothManagerUpdate) + 0x10F;
                    if (pInstr[0] == 0xF3 && pInstr[1] == 0x0F && pInstr[2] == 0x10 && pInstr[3] == 0x05) {
                        int32_t disp32 = 0;
                        std::memcpy(&disp32, pInstr + 4, sizeof(disp32));
                        g_pClothSystemTimeStep = reinterpret_cast<float*>(
                            reinterpret_cast<uintptr_t>(pInstr) + 8 + disp32);
                        LOG(LL_NFO, "sm_systemTime.m_StepInSeconds resolved at ",
                            Logger::hex(reinterpret_cast<uint64_t>(g_pClothSystemTimeStep), 16));
                    } else {
                        LOG(LL_WRN, "ClothManagerUpdate: could not find sm_systemTime movss at expected offset.",
                            " Bytes: ", Logger::hex(pInstr[0],2), " ", Logger::hex(pInstr[1],2),
                            " ", Logger::hex(pInstr[2],2), " ", Logger::hex(pInstr[3],2));
                    }
                    PERFORM_X64_HOOK_WITH_SCHEME_REQUIRED(ClothManagerUpdate, pClothManagerUpdate,
                                                          PLH::x64Detour::detour_scheme_t::INPLACE);
                    LOG(LL_NFO, "ClothManagerUpdate hook installed. trampoline:",
                        reinterpret_cast<void*>(GameHooks::ClothManagerUpdate::OriginalFunc));
                    LOG(LL_DBG, "ClothManagerUpdate: will patch sm_systemTime.m_StepInSeconds to export sub-frame step during export");
                }
            } else {
                LOG(LL_WRN, "Could not find ClothManagerUpdate function.");
                LOG(LL_WRN, "Flag/cloth wind simulation may run too fast during video export.");
            }
            
            if (Config::Manager::disable_watermark) {
                PERFORM_SINGLE_BYTE_PATCH(pWatermarkRenderer, 0xC3, "Watermark renderer");
            } else {
                LOG(LL_DBG, "Watermark renderer not disabled (disable_watermark=false)");
            }
            
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
        }
    } catch (std::exception& ex) {
        // TODO cleanup
        POST();
        throw ex;
    }
    POST();
}

void ever::ScriptMain() {
    PRE();
    mainThreadId = std::this_thread::get_id();
    LOG(LL_NFO, "ScriptHook main thread registered for export handoffs");
    LOG(LL_NFO, "Starting main loop");
    while (true) {
        if (!g_isEnhancedEdition && !isD3D11HookInstalled) {
            ensureD3D11Hook();
        }

        executePass2StartupOnMainThread();

        if (dualPassContext) {
            if (dualPassContext->state == DualPassState::PASS1_COMPLETE ||
                dualPassContext->state == DualPassState::PASS2_PENDING) {
                tryForceWantDelayedCloseFalse("ScriptMain-loop");
            }

            if (dualPassContext->state == DualPassState::PASS1_COMPLETE &&
                !g_pass2TriggerIssued.load() &&
                pCleanupReplayPlaybackOriginal != nullptr) {
                LOG(LL_NFO, "PASS1_COMPLETE detected in ScriptMain. Triggering cleanup gate for pass 2.");
                try {
                    reinterpret_cast<CleanupReplayPlaybackInternalFunc>(pCleanupReplayPlaybackOriginal)();
                    LOG(LL_NFO, "Cleanup trigger dispatched from ScriptMain for pass-2 startup");
                } catch (const std::exception& ex) {
                    LOG(LL_ERR, "ScriptMain pass-2 cleanup trigger exception: ", ex.what());
                    g_pass2TriggerIssued = false;
                } catch (...) {
                    LOG(LL_ERR, "ScriptMain pass-2 cleanup trigger unknown exception");
                    g_pass2TriggerIssued = false;
                }
            }

            if (dualPassContext->state == DualPassState::IDLE ||
                dualPassContext->state == DualPassState::PASS2_COMPLETE) {
                g_pass2TriggerIssued = false;
                g_pass2StartupPending = false;
                g_pass2StartupDelayFrames = 0;
            }
        } else {
            g_pass2TriggerIssued = false;
            g_pass2StartupPending = false;
            g_pass2StartupDelayFrames = 0;
        }
        
        WAIT(0);
    }
}

HMODULE ImportHooks::LoadLibraryW::Implementation(LPCWSTR lp_lib_file_name) {
    PRE();
    const HMODULE result = OriginalFunc(lp_lib_file_name);

    std::wstring libFileName(lp_lib_file_name);
    std::ranges::transform(libFileName, libFileName.begin(), std::towlower);

    if (result != nullptr) {
        try {
            if (!g_isEnhancedEdition && std::wstring(L"d3d11.dll") == libFileName) {
                if (!installD3D11Hook(result)) {
                    LOG(LL_WRN, "Failed to install D3D11 hook from LoadLibraryW path");
                }
            }
        } catch (...) {
            LOG(LL_ERR, "Hooking functions failed");
        }
    }
    POST();
    return result;
}

HMODULE ImportHooks::LoadLibraryA::Implementation(LPCSTR lp_lib_file_name) {
    PRE();
    const HMODULE result = OriginalFunc(lp_lib_file_name);

    if (result != nullptr) {
        try {
            std::string libFileName(lp_lib_file_name);
            std::ranges::transform(libFileName, libFileName.begin(), [](const char c) { return std::tolower(c); });

            if (!g_isEnhancedEdition && std::string("d3d11.dll") == libFileName) {
                if (!installD3D11Hook(result)) {
                    LOG(LL_WRN, "Failed to install D3D11 hook from LoadLibraryA path");
                }
            }
        } catch (...) {
            LOG(LL_ERR, "Hooking functions failed");
        }
    }
    POST();
    return result;
}

LPTOP_LEVEL_EXCEPTION_FILTER
ImportHooks::SetUnhandledExceptionFilter::Implementation(
    LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter)
{
    LOG(LL_WRN, "[CrashHandler] GTA V is calling SetUnhandledExceptionFilter, "
        "replacing our UEF with handler at 0x",
        Logger::hex(reinterpret_cast<uint64_t>(lpTopLevelExceptionFilter), 16));
    LOG(LL_WRN, "[CrashHandler] Our Vectored Exception Handler (VEH) is "
        "UNAFFECTED by this - it will still fire first on any crash.");

    // Let GTA install its handler normally.
    return OriginalFunc(lpTopLevelExceptionFilter);
}

int ImportHooks::MessageBoxW::Implementation(
    HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
{
    // Exact flags used by diagTerminate
    constexpr UINT kRageFatalFlags = MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST;

    if ((uType & kRageFatalFlags) == kRageFatalFlags && lpText) {
        // Convert the wide error string to narrow UTF-8 for the crash handler
        char narrowText[2048]    = {};
        char narrowCaption[512]  = {};
        WideCharToMultiByte(CP_UTF8, 0, lpText,    -1, narrowText,    sizeof(narrowText)    - 1, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, lpCaption, -1, narrowCaption, sizeof(narrowCaption) - 1, nullptr, nullptr);

        char combined[2600];
        snprintf(combined, sizeof(combined), "[%s] %s", narrowCaption, narrowText);

        // Store for the crash handler (thread-safe Interlocked write in RecordRageErrorMessage)
        ever::crash::RecordRageErrorMessage(combined);
    }

    return OriginalFunc(hWnd, lpText, lpCaption, uType);
}

void ID3D11DeviceContextHooks::OMSetRenderTargets::Implementation(ID3D11DeviceContext* p_this, UINT num_views,
                                                                  ID3D11RenderTargetView* const* pp_render_target_views,
                                                                  ID3D11DepthStencilView* p_depth_stencil_view) {
    PRE();
    if (::exportContext) {
        for (uint32_t i = 0; i < num_views; i++) {
            if (pp_render_target_views[i]) {
                ComPtr<ID3D11Resource> pResource;
                ComPtr<ID3D11Texture2D> pTexture2D;
                pp_render_target_views[i]->GetResource(pResource.GetAddressOf());
                if (SUCCEEDED(pResource.As(&pTexture2D))) {
                    if (pTexture2D.Get() == pGameDepthBufferQuarterLinear.Get()) {
                        LOG(LL_DBG, " i:", i, " num:", num_views, " dsv:", static_cast<void*>(p_depth_stencil_view));
                        pCtxLinearizeBuffer = p_this;
                    }
                }
            }
        }
    }

    ComPtr<ID3D11Resource> pRTVTexture;
    if ((pp_render_target_views) && (pp_render_target_views[0])) {
        LOG_CALL(LL_TRC, pp_render_target_views[0]->GetResource(pRTVTexture.GetAddressOf()));
    }

    const bool matchesExportTarget = (::exportContext != nullptr && ::exportContext->p_export_render_target != nullptr &&
                                      (::exportContext->p_export_render_target == pRTVTexture));
    if (matchesExportTarget) {
        if (!bindExportSwapChainIfAvailable()) {
            LOG(LL_TRC, "Swap chain not bound yet; skipping export capture for this frame");
        } else {
            // Skip video capture during Pass 1 (audio-only mode)
            if (dualPassContext && dualPassContext->state == DualPassState::PASS1_RUNNING) {
                LOG(LL_TRC, "Pass 1 (audio-only): Skipping video frame capture");
                // Don't capture video frames - just let the frame render normally
            } else {
                // Pass 2 or normal mode: Capture video frames
                try {
                    ComPtr<ID3D11Device> pDevice;
                    p_this->GetDevice(pDevice.GetAddressOf());

                    ComPtr<ID3D11Texture2D> pDepthBufferCopy = nullptr;
                    ComPtr<ID3D11Texture2D> pBackBufferCopy = nullptr;

            if (Config::Manager::export_openexr) {
                TRY([&] {
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        pLinearDepthTexture->GetDesc(&desc);

                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_FLAG::D3D11_CPU_ACCESS_READ;
                        desc.BindFlags = 0;
                        desc.MiscFlags = 0;
                        desc.Usage = D3D11_USAGE::D3D11_USAGE_STAGING;

                        REQUIRE(pDevice->CreateTexture2D(&desc, NULL, pDepthBufferCopy.GetAddressOf()),
                                "Failed to create depth buffer copy texture");

                        p_this->CopyResource(pDepthBufferCopy.Get(), pLinearDepthTexture.Get());
                    }
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        pGameBackBufferResolved->GetDesc(&desc);
                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_FLAG::D3D11_CPU_ACCESS_READ;
                        desc.BindFlags = 0;
                        desc.MiscFlags = 0;
                        desc.Usage = D3D11_USAGE::D3D11_USAGE_STAGING;

                        REQUIRE(pDevice->CreateTexture2D(&desc, NULL, pBackBufferCopy.GetAddressOf()),
                                "Failed to create back buffer copy texture");

                        p_this->CopyResource(pBackBufferCopy.Get(), pGameBackBufferResolved.Get());
                    }
                    {
                        std::lock_guard<std::mutex> sessionLock(mxSession);
                        if ((encodingSession != nullptr) && (encodingSession->isCapturing)) {
                            encodingSession->enqueueExrImage(p_this, pBackBufferCopy, pDepthBufferCopy);
                        }
                    }
                });
            }

            ComPtr<ID3D11Texture2D> pSwapChainBuffer;
            REQUIRE(::exportContext->p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                             reinterpret_cast<void**>(pSwapChainBuffer.GetAddressOf())),
                    "Failed to get swap chain's buffer");

            LOG_CALL(LL_DBG,
                     ::exportContext->p_swap_chain->Present(1, 0)); // IMPORTANT: This call makes ENB and ReShade
                                                                    // effects to be applied to the render target

            LOG_CALL(LL_DBG,
                     ::exportContext->p_swap_chain->Present(1, 0)); // IMPORTANT: This call makes ENB and ReShade
                                                                    // effects to be applied to the render target

            if (Config::Manager::motion_blur_samples != 0) {
                const float current_shutter_position =
                    (::exportContext->total_frame_num % (Config::Manager::motion_blur_samples + 1)) /
                    static_cast<float>(Config::Manager::motion_blur_samples);

                if (current_shutter_position >= (1 - Config::Manager::motion_blur_strength)) {
                    ever::drawAdditive(pDevice, p_this, pSwapChainBuffer);
                }
            } else {
                // Trick to use the same buffers for when not using motion blur
                ::exportContext->acc_count = 0;
                ever::drawAdditive(pDevice, p_this, pSwapChainBuffer);
                ::exportContext->acc_count = 1;
                ::exportContext->total_frame_num = 1;
            }

            if ((::exportContext->total_frame_num % (::Config::Manager::motion_blur_samples + 1)) ==
                Config::Manager::motion_blur_samples) {

                const ComPtr<ID3D11Texture2D> result = ever::divideBuffer(pDevice, p_this, ::exportContext->acc_count);
                ::exportContext->acc_count = 0;

                D3D11_MAPPED_SUBRESOURCE mapped;

                REQUIRE(p_this->Map(result.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Failed to capture swapbuffer.");
                {
                    std::lock_guard sessionLock(mxSession);
                    if ((encodingSession != nullptr) && (encodingSession->isCapturing)) {
                        REQUIRE(encodingSession->enqueueVideoFrame(mapped), "Failed to enqueue frame.");
                    }
                }
                p_this->Unmap(result.Get(), 0);
            }
            ::exportContext->total_frame_num++;
                } catch (std::exception&) {
                    LOG(LL_ERR, "Reading video frame from D3D Device failed.");
                    LOG_CALL(LL_DBG, encodingSession.reset());
                    LOG_CALL(LL_DBG, ::exportContext.reset());
                }
            } // End of Pass 2/normal mode video capture
        }
    }
    LOG_CALL(LL_TRC, ID3D11DeviceContextHooks::OMSetRenderTargets::OriginalFunc(
                         p_this, num_views, pp_render_target_views, p_depth_stencil_view));
    POST();
}

HRESULT ImportHooks::MFCreateSinkWriterFromURL::Implementation(LPCWSTR pwszOutputURL, IMFByteStream* pByteStream,
                                                               IMFAttributes* pAttributes,
                                                               IMFSinkWriter** ppSinkWriter) {
    PRE();
    const HRESULT result = OriginalFunc(pwszOutputURL, pByteStream, pAttributes, ppSinkWriter);
    if (SUCCEEDED(result)) {
        try {
            if (g_isEnhancedEdition) {
                std::lock_guard<std::mutex> sessionLock(mxSession);
                AutoReloadConfig();
                CreateNewExportContext();
                LOG(LL_NFO, "Enhanced export context created from MFCreateSinkWriterFromURL");
            }

            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, AddStream, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, SetInputMediaType, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, WriteSample, *ppSinkWriter);
            PERFORM_MEMBER_HOOK_REQUIRED(IMFSinkWriter, Finalize, *ppSinkWriter);
        } catch (...) {
            LOG(LL_ERR, "Hooking IMFSinkWriter functions failed");
        }
    }
    POST();
    return result;
}

HRESULT IMFSinkWriterHooks::AddStream::Implementation(IMFSinkWriter* pThis, IMFMediaType* pTargetMediaType,
                                                      DWORD* pdwStreamIndex) {
    PRE();
    LOG(LL_NFO, "IMFSinkWriter::AddStream: ", GetMediaTypeDescription(pTargetMediaType).c_str());
    const HRESULT result = OriginalFunc(pThis, pTargetMediaType, pdwStreamIndex);

    if (SUCCEEDED(result) && pdwStreamIndex != nullptr) {
        std::lock_guard<std::mutex> sessionLock(mxSession);
        if (::exportContext != nullptr) {
            GUID majorType{};
            if (SUCCEEDED(pTargetMediaType->GetMajorType(&majorType))) {
                if (IsEqualGUID(majorType, MFMediaType_Video)) {
                    ::exportContext->video_stream_index = *pdwStreamIndex;
                } else if (IsEqualGUID(majorType, MFMediaType_Audio)) {
                    ::exportContext->audio_stream_index = *pdwStreamIndex;
                }
            }
        }
    }

    POST();
    return result;
}

void CreateMotionBlurBuffers(ComPtr<ID3D11Device> p_device, D3D11_TEXTURE2D_DESC const& desc) {
    D3D11_TEXTURE2D_DESC accBufDesc = desc;
    accBufDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    accBufDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    accBufDesc.CPUAccessFlags = 0;
    accBufDesc.MiscFlags = 0;
    accBufDesc.Usage = D3D11_USAGE::D3D11_USAGE_DEFAULT;
    accBufDesc.MipLevels = 1;
    accBufDesc.ArraySize = 1;
    accBufDesc.SampleDesc.Count = 1;
    accBufDesc.SampleDesc.Quality = 0;

    LOG_IF_FAILED(p_device->CreateTexture2D(&accBufDesc, NULL, pMotionBlurAccBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create accumulation buffer texture");

    D3D11_TEXTURE2D_DESC mbBufferDesc = accBufDesc;
    mbBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    mbBufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    LOG_IF_FAILED(p_device->CreateTexture2D(&mbBufferDesc, NULL, pMotionBlurFinalBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create motion blur buffer texture");

    D3D11_RENDER_TARGET_VIEW_DESC accBufRTVDesc;
    accBufRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    accBufRTVDesc.Texture2D.MipSlice = 0;
    accBufRTVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    LOG_IF_FAILED(p_device->CreateRenderTargetView(pMotionBlurAccBuffer.Get(), &accBufRTVDesc,
                                                   pRtvAccBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create acc buffer RTV.");

    D3D11_RENDER_TARGET_VIEW_DESC mbBufferRTVDesc;
    mbBufferRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    mbBufferRTVDesc.Texture2D.MipSlice = 0;
    mbBufferRTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    LOG_IF_FAILED(p_device->CreateRenderTargetView(pMotionBlurFinalBuffer.Get(), &mbBufferRTVDesc,
                                                   pRtvBlurBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create blur buffer RTV.");

    D3D11_SHADER_RESOURCE_VIEW_DESC accBufSRVDesc;
    accBufSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    accBufSRVDesc.Texture2D.MipLevels = 1;
    accBufSRVDesc.Texture2D.MostDetailedMip = 0;
    accBufSRVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    LOG_IF_FAILED(p_device->CreateShaderResourceView(pMotionBlurAccBuffer.Get(), &accBufSRVDesc,
                                                     pSrvAccBuffer.ReleaseAndGetAddressOf()),
                  "Failed to create blur buffer SRV.");

    D3D11_TEXTURE2D_DESC sourceSRVTextureDesc = desc;
    sourceSRVTextureDesc.CPUAccessFlags = 0;
    sourceSRVTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sourceSRVTextureDesc.MiscFlags = 0;
    sourceSRVTextureDesc.Usage = D3D11_USAGE::D3D11_USAGE_DEFAULT;
    sourceSRVTextureDesc.ArraySize = 1;
    sourceSRVTextureDesc.SampleDesc.Count = 1;
    sourceSRVTextureDesc.SampleDesc.Quality = 0;
    sourceSRVTextureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sourceSRVTextureDesc.MipLevels = 1;

    REQUIRE(p_device->CreateTexture2D(&sourceSRVTextureDesc, NULL, pSourceSrvTexture.ReleaseAndGetAddressOf()),
            "Failed to create back buffer copy texture");

    D3D11_SHADER_RESOURCE_VIEW_DESC sourceSRVDesc;
    sourceSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sourceSRVDesc.Texture2D.MipLevels = 1;
    sourceSRVDesc.Texture2D.MostDetailedMip = 0;
    sourceSRVDesc.Format = sourceSRVTextureDesc.Format;

    LOG_IF_FAILED(p_device->CreateShaderResourceView(pSourceSrvTexture.Get(), &sourceSRVDesc,
                                                     pSourceSrv.ReleaseAndGetAddressOf()),
                  "Failed to create backbuffer copy SRV.");
}

HRESULT IMFSinkWriterHooks::SetInputMediaType::Implementation(IMFSinkWriter* pThis, DWORD dwStreamIndex,
                                                              IMFMediaType* pInputMediaType,
                                                              IMFAttributes* pEncodingParameters) {
    PRE();
    LOG(LL_NFO, "IMFSinkWriter::SetInputMediaType: ", GetMediaTypeDescription(pInputMediaType).c_str());

    if (::exportContext == nullptr || encodingSession == nullptr) {
        LOG(LL_ERR, "SetInputMediaType called without an active EVER export context");
        POST();
        return OriginalFunc(pThis, dwStreamIndex, pInputMediaType, pEncodingParameters);
    }

    GUID majorType;
    if (SUCCEEDED(pInputMediaType->GetMajorType(&majorType))) {
        if (IsEqualGUID(majorType, MFMediaType_Video)) {
            ::exportContext->video_media_type = pInputMediaType;
            if (g_isEnhancedEdition) {
                UINT32 width = 0;
                UINT32 height = 0;
                if (SUCCEEDED(MFGetAttributeSize(pInputMediaType, MF_MT_FRAME_SIZE, &width, &height))) {
                    const uint32_t runtimeWidth = enhancedRuntimeWidth.load(std::memory_order_relaxed);
                    const uint32_t runtimeHeight = enhancedRuntimeHeight.load(std::memory_order_relaxed);
                    const uint32_t exportWidth = runtimeWidth != 0 ? runtimeWidth : width;
                    const uint32_t exportHeight = runtimeHeight != 0 ? runtimeHeight : height;
                    enhancedExpectedWidth = exportWidth;
                    enhancedExpectedHeight = exportHeight;
                    LOG(LL_DBG, "Enhanced export resource probe armed for ", exportWidth, "x", exportHeight,
                        " (Rockstar stream ", width, "x", height, ")");
                }
            }
        } else if (IsEqualGUID(majorType, MFMediaType_Audio)) {
            try {
                std::lock_guard<std::mutex> sessionLock(mxSession);
                UINT width, height, fps_num, fps_den;
                MFGetAttribute2UINT32asUINT64(::exportContext->video_media_type.Get(), MF_MT_FRAME_SIZE, &width,
                                              &height);
                MFGetAttributeRatio(::exportContext->video_media_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);

                GUID pixelFormat;
                ::exportContext->video_media_type->GetGUID(MF_MT_SUBTYPE, &pixelFormat);

                if (isCustomFrameRateSupported) {
                    auto fps = Config::Manager::fps;
                    fps_num = fps.first;
                    fps_den = fps.second;

                    float gameFrameRate =
                        (static_cast<float>(fps.first) * (static_cast<float>(Config::Manager::motion_blur_samples) + 1) /
                         static_cast<float>(fps.second));
                    
                    LOG(LL_DBG, "Effective frame rate: ", gameFrameRate, " FPS");
                    if (g_isEnhancedEdition && gameFrameRate > 60.0f) {
                        LOG(isEnhancedDualPassSupported ? LL_NFO : LL_WRN,
                            "Enhanced export is configured above the Legacy single-pass audio limit. effectiveFps=",
                            gameFrameRate, " dualPassAvailable=", isEnhancedDualPassSupported,
                            isEnhancedDualPassSupported
                                ? "; audio-only/video-only workflow active"
                                : "; audio support is not validated for this configuration");
                    }
                    
                    if (dualPassContext) {
                        LOG(LL_DBG, "Dual-pass state: ", static_cast<int>(dualPassContext->state));
                    }
                }

                UINT32 blockAlignment, numChannels, sampleRate, bitsPerSample;
                GUID subType;

                pInputMediaType->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlignment);
                pInputMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &numChannels);
                pInputMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
                pInputMediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
                pInputMediaType->GetGUID(MF_MT_SUBTYPE, &subType);

                char buffer[128];
                std::string output_file = Config::Manager::output_dir + "\\EVER-";
                
                // Dual-pass: Use saved timestamp for Pass 2, generate new one for Pass 1
                std::string timestamp_str;
                if (dualPassContext && 
                    (dualPassContext->state == DualPassState::PASS2_PENDING || 
                     dualPassContext->state == DualPassState::PASS2_RUNNING) && 
                    !dualPassContext->timestamp.empty()) {
                    // Pass 2: Reuse timestamp from Pass 1
                    timestamp_str = dualPassContext->timestamp;
                    LOG(LL_DBG, "Pass 2: Reusing timestamp from Pass 1: ", timestamp_str);
                } else {
                    auto now = std::chrono::system_clock::now();
                    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
                    auto epoch = now_ms.time_since_epoch();
                    auto value = std::chrono::duration_cast<std::chrono::milliseconds>(epoch);
                    long long milliseconds = value.count() % 1000;
                    
                    time_t rawtime = std::chrono::system_clock::to_time_t(now);
                    struct tm timeinfo;
                    localtime_s(&timeinfo, &rawtime);
                    strftime(buffer, 128, "%Y%m%d%H%M%S", &timeinfo);
                    
                    timestamp_str = std::string(buffer) + "_" + std::to_string(milliseconds);
                    
                    if (dualPassContext && dualPassContext->state == DualPassState::PASS1_RUNNING) {
                        // Save timestamp for Pass 2
                        dualPassContext->timestamp = timestamp_str;
                        LOG(LL_DBG, "Pass 1: Saved timestamp for Pass 2: ", timestamp_str);
                    }
                }
                
                output_file += timestamp_str;
                
                std::string filename;
                if (dualPassContext) {
                    LOG(LL_DBG, "SetInputMediaType: dualPassContext state = ", static_cast<int>(dualPassContext->state));
                    if (dualPassContext->state == DualPassState::PASS1_RUNNING) {
                        // Pass 1: audio only (in-memory capture)
                        std::string extension = outputExtensionForContainer(Config::Manager::encoder_config.format.container);
                        dualPassContext->final_output_file = output_file + extension;
                        LOG(LL_NFO, "Pass 1 audio output: IN-MEMORY BUFFER");
                        LOG(LL_NFO, "Pass 1 final output will be: ", dualPassContext->final_output_file);
                        
                        // Initialize audio buffer metadata
                        dualPassContext->audio_sample_rate = sampleRate;
                        dualPassContext->audio_channels = static_cast<uint16_t>(numChannels);
                        dualPassContext->audio_bits_per_sample = static_cast<uint16_t>(bitsPerSample);
                        dualPassContext->audio_block_align = blockAlignment;
                        dualPassContext->audio_buffer.clear();
                        dualPassContext->audio_buffer.reserve(1024 * 1024 * 64); // Reserve 64MB initially
                        dualPassContext->audio_buffer_playback_position = 0;
                        LOG(LL_NFO, "Audio buffer initialized: ", sampleRate, "Hz, ", numChannels, " channels, ", bitsPerSample, " bits");
                    } else if (dualPassContext->state == DualPassState::PASS2_PENDING || 
                               dualPassContext->state == DualPassState::PASS2_RUNNING) {
                        // Pass 2: video + buffered audio -> final output
                        filename = dualPassContext->final_output_file;
                        LOG(LL_NFO, "Pass 2 output (video + buffered audio): ", filename);
                        LOG(LL_NFO, "  Buffered audio size: ", dualPassContext->audio_buffer.size(), " bytes");
                    } else {
                        LOG(LL_DBG, "SetInputMediaType: State not PASS1 or PASS2, using default filename");
                        std::string extension = outputExtensionForContainer(Config::Manager::encoder_config.format.container);
                        filename = output_file + extension;
                    }
                } else {
                    LOG(LL_DBG, "SetInputMediaType: No dualPassContext, using default filename");
                    std::string extension = outputExtensionForContainer(Config::Manager::encoder_config.format.container);
                    filename = output_file + extension;
                }

                LOG(LL_NFO, "Output file: ", filename);

                // Skip FFmpeg encoder creation during Pass 1 (audio-only mode)
                // We only buffer audio in memory during Pass 1
                if (dualPassContext && dualPassContext->state == DualPassState::PASS1_RUNNING) {
                    LOG(LL_NFO, "Pass 1 (audio-only): Skipping FFmpeg encoder creation");
                    LOG(LL_NFO, "Audio will be buffered in memory");
                    // Don't create encoder context - audio buffer already initialized above
                } else {
                    // Pass 2 or normal mode: Create full FFmpeg encoder (video + audio)
                    if (dualPassContext && 
                        (dualPassContext->state == DualPassState::PASS2_PENDING || 
                         dualPassContext->state == DualPassState::PASS2_RUNNING)) {
                        LOG(LL_NFO, "Pass 2: Creating FFmpeg encoder for video + buffered audio replay");
                    }
                    if (g_isEnhancedEdition) {
                        const uint32_t exportWidth = enhancedRuntimeWidth.load(std::memory_order_relaxed);
                        const uint32_t exportHeight = enhancedRuntimeHeight.load(std::memory_order_relaxed);
                        if (exportWidth == 0 || exportHeight == 0) {
                            throw std::runtime_error(
                                "Enhanced ReShade runtime resolution is unavailable; refusing to silently use the Rockstar export limit");
                        }

                        enhancedExpectedWidth = exportWidth;
                        enhancedExpectedHeight = exportHeight;
                        LOG(LL_NFO, "Enhanced Video Export Resolution: ", exportWidth, "x", exportHeight,
                            " (Rockstar Media Foundation stream: ", width, "x", height, ")");

                        if (Config::Manager::export_openexr) {
                            LOG(LL_NFO, "Enhanced experimental OpenEXR export enabled with synchronized ",
                                "full-resolution D3D12 color/depth readback");
                        }
                        if (Config::Manager::motion_blur_samples != 0) {
                            if (isCustomFrameRateSupported) {
                                LOG(LL_NFO, "Enhanced multi-sample motion blur requested. samples=",
                                    static_cast<uint32_t>(Config::Manager::motion_blur_samples),
                                    " strength=", Config::Manager::motion_blur_strength);
                            } else {
                                LOG(LL_WRN, "Enhanced multi-sample motion blur requires the custom FPS hook; ",
                                    "capturing one native Rockstar Editor frame per output frame");
                            }
                        }

                        REQUIRE(encodingSession->createContext(
                                    Config::Manager::encoder_config, std::wstring(filename.begin(), filename.end()),
                                    exportWidth, exportHeight, "rgba", fps_num, fps_den, numChannels, sampleRate, "s16",
                                    blockAlignment, Config::Manager::export_openexr,
                                    exportWidth, exportHeight),
                                "Failed to create Enhanced encoding context.");
                    } else {
                        DXGI_SWAP_CHAIN_DESC desc{};
                        if (bindExportSwapChainIfAvailable()) {
                            ::exportContext->p_swap_chain->GetDesc(&desc);
                        } else if (::exportContext->p_export_render_target) {
                            D3D11_TEXTURE2D_DESC targetDesc;
                            ::exportContext->p_export_render_target->GetDesc(&targetDesc);
                            desc.BufferDesc.Width = targetDesc.Width;
                            desc.BufferDesc.Height = targetDesc.Height;
                            LOG(LL_WRN, "Swap chain not available during SetInputMediaType; using export texture dimensions");
                        } else {
                            throw std::runtime_error("Export context missing swap chain and render target");
                        }

                        const auto exportWidth = desc.BufferDesc.Width;
                        const auto exportHeight = desc.BufferDesc.Height;

                        // Get pBackBufferResolved dimensions for OpenEXR export
                        D3D11_TEXTURE2D_DESC backBufferCopyDesc;
                        pGameBackBufferResolved->GetDesc(&backBufferCopyDesc);
                        const auto openExrWidth = backBufferCopyDesc.Width;
                        const auto openExrHeight = backBufferCopyDesc.Height;

                        LOG(LL_DBG, "Video Export Resolution: ", exportWidth, "x", exportHeight);
                        LOG(LL_DBG, "OpenEXR Export Resolution: ", openExrWidth, "x", openExrHeight);

                        D3D11_TEXTURE2D_DESC motionBlurBufferDesc;
                        motionBlurBufferDesc.Width = exportWidth;
                        motionBlurBufferDesc.Height = exportHeight;
                        motionBlurBufferDesc.MipLevels = 1;
                        motionBlurBufferDesc.ArraySize = 1;
                        motionBlurBufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        motionBlurBufferDesc.SampleDesc.Count = 1;
                        motionBlurBufferDesc.SampleDesc.Quality = 0;
                        motionBlurBufferDesc.Usage = D3D11_USAGE_DEFAULT;
                        motionBlurBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        motionBlurBufferDesc.CPUAccessFlags = 0;
                        motionBlurBufferDesc.MiscFlags = 0;

                        CreateMotionBlurBuffers(::exportContext->p_device, motionBlurBufferDesc);

                        REQUIRE(encodingSession->createContext(
                                    Config::Manager::encoder_config, std::wstring(filename.begin(), filename.end()),
                                    exportWidth, exportHeight, "rgba", fps_num, fps_den, numChannels, sampleRate,
                                    "s16", blockAlignment, Config::Manager::export_openexr, openExrWidth, openExrHeight),
                                "Failed to create encoding context.");
                    }
                }
            } catch (std::exception& ex) {
                LOG(LL_ERR, ex.what());
                markFfmpegExportFailure("SetInputMediaType", MF_E_INVALIDMEDIATYPE);
                LOG_CALL(LL_DBG, encodingSession.reset());
                LOG_CALL(LL_DBG, ::exportContext.reset());
                
                // Reset dual-pass context on error to prevent stale state
                if (dualPassContext && dualPassContext->state != DualPassState::IDLE) {
                    LOG(LL_ERR, "Resetting dual-pass context due to error");
                    dualPassContext->state = DualPassState::IDLE;
                    dualPassContext->audio_buffer.clear();
                    dualPassContext->audio_buffer.shrink_to_fit();
                    dualPassContext->saved_video_editor_interface = nullptr;
                    dualPassContext->saved_montage_ptr = nullptr;
                }
                
                return MF_E_INVALIDMEDIATYPE;
            }
        }
    }

    POST();
    return OriginalFunc(pThis, dwStreamIndex, pInputMediaType, pEncodingParameters);
}

HRESULT IMFSinkWriterHooks::WriteSample::Implementation(IMFSinkWriter* pThis, DWORD dwStreamIndex, IMFSample* pSample) {
    std::lock_guard sessionLock(mxSession);

    if (g_isEnhancedEdition && encodingSession && ::exportContext &&
        dwStreamIndex == ::exportContext->video_stream_index) {
        if (dualPassContext && dualPassContext->state == DualPassState::PASS1_RUNNING) {
            LOG(LL_TRC, "Enhanced Pass 1 (audio-only): discarding Rockstar video sample");
            return S_OK;
        }
        const uint32_t pending = enhancedPendingVideoFrames.fetch_add(1, std::memory_order_release) + 1;
        LOG(LL_TRC, "Enhanced video sample queued for ReShade capture. pending=", pending);
        return S_OK;
    }

    if (encodingSession && ::exportContext &&
        dwStreamIndex == ::exportContext->audio_stream_index &&
        !::exportContext->is_audio_export_disabled) {

        ComPtr<IMFMediaBuffer> pBuffer = nullptr;
        try {
            LONGLONG sampleTime;
            pSample->GetSampleTime(&sampleTime);
            pSample->ConvertToContiguousBuffer(pBuffer.GetAddressOf());

            DWORD length;
            pBuffer->GetCurrentLength(&length);
            BYTE* buffer;
            if (SUCCEEDED(pBuffer->Lock(&buffer, NULL, NULL))) {
                try {
                    // Check if we're in Pass 1 - append to memory buffer
                    if (dualPassContext && dualPassContext->state == DualPassState::PASS1_RUNNING) {
                        // Append raw PCM samples to in-memory buffer (grows dynamically)
                        size_t old_size = dualPassContext->audio_buffer.size();
                        dualPassContext->audio_buffer.resize(old_size + length);
                        std::memcpy(dualPassContext->audio_buffer.data() + old_size, buffer, length);
                        LOG(LL_TRC, "Pass 1: Buffered ", length, " bytes of audio (total: ", dualPassContext->audio_buffer.size(), " bytes)");
                    } else if (dualPassContext && 
                               (dualPassContext->state == DualPassState::PASS2_PENDING || 
                                dualPassContext->state == DualPassState::PASS2_RUNNING)) {
                        // Pass 2: Replay buffered audio from Pass 1
                        size_t bytes_remaining = dualPassContext->audio_buffer.size() - dualPassContext->audio_buffer_playback_position;
                        
                        if (bytes_remaining > 0) {
                            // Send the same chunk size as we're receiving (or what's left)
                            size_t chunk_size = std::min(static_cast<size_t>(length), bytes_remaining);

                            BYTE* buffered_audio = dualPassContext->audio_buffer.data() + dualPassContext->audio_buffer_playback_position;
                            LOG_CALL(LL_DBG, encodingSession->writeAudioFrame(
                                                 buffered_audio,
                                                 static_cast<int32_t>(chunk_size),
                                                 sampleTime));
                            
                            dualPassContext->audio_buffer_playback_position += chunk_size;
                            LOG(LL_TRC, "Pass 2: Sent ", chunk_size, " bytes from buffer (pos: ", 
                                dualPassContext->audio_buffer_playback_position, "/", dualPassContext->audio_buffer.size(), ")");
                        } else {
                            LOG(LL_WRN, "Pass 2: Audio buffer exhausted! No more audio to send.");
                        }
                    } else {
                        // Normal mode (no dual-pass) - send live audio to FFmpeg
                        LOG_CALL(LL_DBG, encodingSession->writeAudioFrame(
                                             buffer, static_cast<int32_t>(length), sampleTime));
                    }
                } catch (std::exception& ex) {
                    LOG(LL_ERR, ex.what());
                    markFfmpegExportFailure("WriteSample::audio-frame", E_FAIL);
                }
                pBuffer->Unlock();
            }
        } catch (std::exception& ex) {
            LOG(LL_ERR, ex.what());
            markFfmpegExportFailure("WriteSample::exception", E_FAIL);
            LOG_CALL(LL_DBG, encodingSession.reset());
            LOG_CALL(LL_DBG, ::exportContext.reset());
            
            // Reset dual-pass context on error to prevent stale state
            if (dualPassContext && dualPassContext->state != DualPassState::IDLE) {
                LOG(LL_ERR, "Resetting dual-pass context due to WriteSample error");
                dualPassContext->state = DualPassState::IDLE;
                dualPassContext->audio_buffer.clear();
                dualPassContext->audio_buffer.shrink_to_fit();
            }
        }
    }
    return S_OK;
}

HRESULT IMFSinkWriterHooks::Finalize::Implementation(IMFSinkWriter* pThis) {
    PRE();
    
    // Check dual-pass state BEFORE acquiring lock or resetting anything
    LOG(LL_NFO, "IMFSinkWriter::Finalize called");
    if (dualPassContext) {
        LOG(LL_NFO, "  Current state: ", static_cast<int>(dualPassContext->state),
            " (",
            (dualPassContext->state == DualPassState::IDLE ? "IDLE" :
             dualPassContext->state == DualPassState::PASS1_RUNNING ? "PASS1_RUNNING" :
             dualPassContext->state == DualPassState::PASS1_COMPLETE ? "PASS1_COMPLETE" :
             dualPassContext->state == DualPassState::PASS2_PENDING ? "PASS2_PENDING" :
             dualPassContext->state == DualPassState::PASS2_RUNNING ? "PASS2_RUNNING" :
             dualPassContext->state == DualPassState::PASS2_COMPLETE ? "PASS2_COMPLETE" : "UNKNOWN"),
            ")");
    } else {
        LOG(LL_NFO, "  dualPassContext exists: NO (normal single-pass mode)");
    }
    
    std::lock_guard<std::mutex> sessionLock(mxSession);
    HRESULT originalFinalizeResult = S_OK;
    bool originalFinalizeCalled = false;
    bool ffmpegFinalizeSuccess = !ffmpegExportFailed.load();

    try {
        if (encodingSession != NULL) {
            // Check if this is Pass 1 completing
            if (dualPassContext && dualPassContext->state == DualPassState::PASS1_RUNNING) {
                LOG(LL_NFO, "PASS 1 COMPLETE - Audio Capture Finished");
                LOG(LL_NFO, "  Total audio buffered: ", dualPassContext->audio_buffer.size(), " bytes");
                
                if (dualPassContext->audio_buffer.size() > 0) {
                    double durationSeconds = static_cast<double>(dualPassContext->audio_buffer.size()) / 
                                            dualPassContext->audio_block_align / 
                                            dualPassContext->audio_sample_rate;
                    LOG(LL_NFO, "  Audio duration: ~", durationSeconds, " seconds");
                } else {
                    LOG(LL_WRN, "  Warning: No audio was captured in Pass 1!");
                }
                
                // Skip FFmpeg finalization - encoder was never created for Pass 1 (audio-only mode)
                LOG(LL_NFO, "Skipping FFmpeg finalization (no encoder in Pass 1)");
                
                // Mark Pass 1 as complete
                dualPassContext->state = DualPassState::PASS1_COMPLETE;
                LOG(LL_NFO, "State changed to PASS1_COMPLETE");
                
                // Skip FFmpeg finalization (no encoder in Pass 1)
                LOG(LL_NFO, "Skipping FFmpeg finalization (no encoder in Pass 1)");
                
                // Call original to trigger GTA's cleanup
                LOG(LL_DBG, "Executing: OriginalFunc(pThis)...");
                HRESULT result = OriginalFunc(pThis);
                originalFinalizeResult = result;
                originalFinalizeCalled = true;
                
                LOG(LL_NFO, "Original Finalize returned: ", Logger::hex(result, 8));
                LOG(LL_NFO, "Pass 1: overriding Finalize return to S_OK for handoff stability");
                LOG(LL_NFO, "Recording instance should be preserved for Pass 2");
                LOG(LL_NFO, "Waiting for CleanupReplayPlaybackInternal to trigger Pass 2...");
                
                LOG_CALL(LL_DBG, encodingSession.reset());
                LOG_CALL(LL_DBG, ::exportContext.reset());
                resetEnhancedCaptureTracking();

                if (dualPassContext && dualPassContext->state == DualPassState::PASS1_COMPLETE) {
                    LOG(LL_NFO, "Pass 1 finalize: waiting for cleanup hook/ScriptMain to complete handoff to Pass 2");
                }

                LOG(LL_NFO, "IMFSinkWriter::Finalize exit (Pass 1)");
                POST();
                return S_OK;
                
            } else if (dualPassContext && dualPassContext->state == DualPassState::PASS2_RUNNING) {
                // Pass 2 complete
                LOG(LL_NFO, "PASS 2 COMPLETE - Video Export Finished");

                if (encodingSession && dualPassContext->audio_buffer_playback_position < dualPassContext->audio_buffer.size()) {
                    const size_t bytesRemaining =
                        dualPassContext->audio_buffer.size() - dualPassContext->audio_buffer_playback_position;
                    const uint32_t blockAlign = std::max<uint32_t>(1, dualPassContext->audio_block_align);
                    const size_t chunkAlign = static_cast<size_t>(blockAlign);
                    const size_t rawChunk = static_cast<size_t>(blockAlign) * 1024;

                    LOG(LL_NFO, "Draining remaining buffered audio before async finalization. remainingBytes=",
                        bytesRemaining, " blockAlign=", blockAlign);

                    while (dualPassContext->audio_buffer_playback_position < dualPassContext->audio_buffer.size()) {
                        const size_t pending = dualPassContext->audio_buffer.size() - dualPassContext->audio_buffer_playback_position;
                        size_t chunkSize = std::min(rawChunk, pending);
                        chunkSize = (chunkSize / chunkAlign) * chunkAlign;
                        if (chunkSize == 0) {
                            // Keep alignment guarantees for PCM frame boundaries.
                            chunkSize = pending - (pending % chunkAlign);
                        }
                        if (chunkSize == 0) {
                            break;
                        }

                        BYTE* bufferedAudio = dualPassContext->audio_buffer.data() + dualPassContext->audio_buffer_playback_position;
                        const HRESULT drainHr = encodingSession->writeAudioFrame(
                            bufferedAudio,
                            static_cast<int32_t>(chunkSize),
                            0);

                        if (FAILED(drainHr)) {
                            ffmpegFinalizeSuccess = false;
                            markFfmpegExportFailure("Finalize::drainRemainingAudio", drainHr);
                            LOG(LL_ERR, "Failed while draining remaining buffered audio");
                            break;
                        }

                        dualPassContext->audio_buffer_playback_position += chunkSize;
                    }

                    const size_t postDrainRemaining =
                        dualPassContext->audio_buffer.size() - dualPassContext->audio_buffer_playback_position;
                    if (postDrainRemaining > 0) {
                        LOG(LL_WRN, "Audio drain ended with non-aligned remainder bytes=", postDrainRemaining);
                    } else {
                        LOG(LL_NFO, "Buffered audio drain complete");
                    }
                }

                std::unique_ptr<Encoder::EncoderSession> sessionForAsyncFinalize;
                sessionForAsyncFinalize = std::move(encodingSession);
                if (!startAsyncEncoderFinalization(std::move(sessionForAsyncFinalize), "FinalizeAsync::pass2")) {
                    ffmpegFinalizeSuccess = false;
                }
                
                // Mark as complete BEFORE calling original Finalize
                LOG(LL_NFO, "DUAL-PASS RENDERING COMPLETE");
                LOG(LL_NFO, "  Final output: ", dualPassContext->final_output_file);
                LOG(LL_NFO, "Resetting dual-pass context");
                dualPassContext->audio_buffer.clear();
                dualPassContext->audio_buffer.shrink_to_fit();
                dualPassContext->saved_video_editor_interface = nullptr;
                dualPassContext->saved_montage_ptr = nullptr;
                dualPassContext->timestamp.clear();
                dualPassContext->final_output_file.clear();
                
                // Mark as complete and reset context
                LOG(LL_NFO, "Changing state from PASS2_RUNNING to PASS2_COMPLETE");
                dualPassContext->state = DualPassState::PASS2_COMPLETE;
                LOG(LL_NFO, "State changed: PASS2_COMPLETE");
                LOG(LL_DBG, "  KillPlaybackOrBake hook to execute normally when GTA calls it");
                
                // Clean up our resources
                LOG(LL_DBG, "Cleaning up EVER resources...");
                LOG_CALL(LL_DBG, ::exportContext.reset());
                resetEnhancedCaptureTracking();
                LOG(LL_DBG, "EVER resources cleaned up");

                LOG(LL_DBG, "Executing OriginalFunc(pThis) for PASS2 cleanup...");
                originalFinalizeResult = OriginalFunc(pThis);
                originalFinalizeCalled = true;
                LOG(LL_NFO, "Original Finalize (Pass 2) returned: ", Logger::hex(originalFinalizeResult, 8));
                
                LOG(LL_NFO, "IMFSinkWriter::Finalize exit (Pass 2)");
                POST();
                if (ffmpegFinalizeSuccess && !ffmpegExportFailed.load()) {
                    // Async completion will publish final success/failure state.
                    return S_OK;
                }

                LOG(LL_ERR, "Returning E_FAIL from Pass 2 finalize because FFmpeg export failed. stage=",
                    getFfmpegExportFailureStage().empty() ? "<unknown>" : getFfmpegExportFailureStage());
                return E_FAIL;
                
            } else {
                // Normal mode or other states - finalize encoder
                if (dualPassContext) {
                    LOG(LL_NFO, "Finalizing encoder (current state: ", static_cast<int>(dualPassContext->state), ")");
                } else {
                    LOG(LL_NFO, "Finalizing encoder (normal single-pass mode)");
                }

                if (g_isEnhancedEdition) {
                    const HRESULT drainVideoHr = drainEnhancedReadbacksForFinalize();
                    if (FAILED(drainVideoHr)) {
                        ffmpegFinalizeSuccess = false;
                        markFfmpegExportFailure("Finalize::normal::drainEnhancedReadbacks", drainVideoHr);
                    }
                }

                if (g_isEnhancedEdition) {
                    // GTA Enhanced can access stale bake state if its Media Foundation Finalize
                    // callback is blocked for a long software-encoder/OpenEXR drain. Let Rockstar's
                    // own Finalize return promptly and keep its loading gate active through the
                    // existing async-finalization status hooks until the trailer is durable.
                    std::unique_ptr<Encoder::EncoderSession> sessionForAsyncFinalize =
                        std::move(encodingSession);
                    if (!startAsyncEncoderFinalization(
                            std::move(sessionForAsyncFinalize), "FinalizeAsync::single-pass")) {
                        ffmpegFinalizeSuccess = false;
                    }
                } else {
                    const HRESULT finishAudioHr = encodingSession->finishAudio();
                    const HRESULT finishVideoHr = encodingSession->finishVideo();
                    const HRESULT endSessionHr = encodingSession->endSession();

                    if (FAILED(finishAudioHr)) {
                        ffmpegFinalizeSuccess = false;
                        markFfmpegExportFailure("Finalize::normal::finishAudio", finishAudioHr);
                    }
                    if (FAILED(finishVideoHr)) {
                        ffmpegFinalizeSuccess = false;
                        markFfmpegExportFailure("Finalize::normal::finishVideo", finishVideoHr);
                    }
                    if (FAILED(endSessionHr)) {
                        ffmpegFinalizeSuccess = false;
                        markFfmpegExportFailure("Finalize::normal::endSession", endSessionHr);
                    }
                }
            }
        }
    } catch (std::exception& ex) {
        LOG(LL_ERR, "Exception in Finalize: ", ex.what());
        ffmpegFinalizeSuccess = false;
        markFfmpegExportFailure("Finalize::exception", E_FAIL);
        
        // Reset dual-pass context on error to prevent stale state
        if (dualPassContext && dualPassContext->state != DualPassState::IDLE) {
            LOG(LL_ERR, "Resetting dual-pass context due to Finalize error");
            dualPassContext->state = DualPassState::IDLE;
            dualPassContext->audio_buffer.clear();
            dualPassContext->audio_buffer.shrink_to_fit();
            dualPassContext->saved_video_editor_interface = nullptr;
            dualPassContext->saved_montage_ptr = nullptr;
        }
    }

    LOG_CALL(LL_DBG, encodingSession.reset());
    LOG_CALL(LL_DBG, ::exportContext.reset());
    resetEnhancedCaptureTracking();
    
    LOG(LL_NFO, "=== IMFSinkWriter::Finalize exit ===");
    POST();
    
    // Call original Finalize to let GTA clean up
    if (!originalFinalizeCalled) {
        originalFinalizeResult = OriginalFunc(pThis);
        originalFinalizeCalled = true;
    }

    LOG(LL_NFO, "Original Finalize returned: ", Logger::hex(originalFinalizeResult, 8));

    if (ffmpegFinalizeSuccess && !ffmpegExportFailed.load()) {
        clearFfmpegExportFailure();
        return S_OK;
    }

    LOG(LL_ERR, "Returning E_FAIL from Finalize because FFmpeg export failed. stage=",
        getFfmpegExportFailureStage().empty() ? "<unknown>" : getFfmpegExportFailureStage());
    return E_FAIL;
}

void ever::finalize() {
    PRE();
    {
        std::lock_guard<std::mutex> captureLock(reshadeCaptureMutex);
        reshadeCapturePixels.clear();
        reshadeCapturePixels.shrink_to_fit();
    }
    POST();
}

bool ever::isExportActive() {
    // Check if an export session is currently active
    std::lock_guard<std::mutex> lock(mxSession);
    return encodingSession != nullptr && encodingSession->isCapturing;
}

void ID3D11DeviceContextHooks::Draw::Implementation(ID3D11DeviceContext* pThis, //
                                                    UINT VertexCount,           //
                                                    UINT StartVertexLocation) {
    OriginalFunc(pThis, VertexCount, StartVertexLocation);
    if (pCtxLinearizeBuffer == pThis) {
        pCtxLinearizeBuffer = nullptr;

        ComPtr<ID3D11Device> pDevice;
        pThis->GetDevice(pDevice.GetAddressOf());

        ComPtr<ID3D11RenderTargetView> pCurrentRTV;
        LOG_CALL(LL_DBG, pThis->OMGetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        pCurrentRTV->GetDesc(&rtvDesc);
        LOG_CALL(LL_DBG, pDevice->CreateRenderTargetView(pLinearDepthTexture.Get(), &rtvDesc,
                                                         pCurrentRTV.ReleaseAndGetAddressOf()));
        LOG_CALL(LL_DBG, pThis->OMSetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));

        D3D11_TEXTURE2D_DESC ldtDesc;
        pLinearDepthTexture->GetDesc(&ldtDesc);

        D3D11_VIEWPORT viewport;
        viewport.Width = static_cast<float>(ldtDesc.Width);
        viewport.Height = static_cast<float>(ldtDesc.Height);
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.MinDepth = 0;
        viewport.MaxDepth = 1;
        pThis->RSSetViewports(1, &viewport);
        D3D11_RECT rect;
        rect.left = rect.top = 0;
        rect.right = static_cast<LONG>(ldtDesc.Width);
        rect.bottom = static_cast<LONG>(ldtDesc.Height);
        pThis->RSSetScissorRects(1, &rect);

        LOG_CALL(LL_TRC, OriginalFunc(pThis, VertexCount, StartVertexLocation));
    }
}

void ID3D11DeviceContextHooks::DrawIndexed::Implementation(ID3D11DeviceContext* pThis, //
                                                           UINT IndexCount,            //
                                                           UINT StartIndexLocation,    //
                                                           INT BaseVertexLocation) {
    OriginalFunc(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
    if (pCtxLinearizeBuffer == pThis) {
        pCtxLinearizeBuffer = nullptr;

        ComPtr<ID3D11Device> pDevice;
        pThis->GetDevice(pDevice.GetAddressOf());

        ComPtr<ID3D11RenderTargetView> pCurrentRTV;
        LOG_CALL(LL_DBG, pThis->OMGetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        pCurrentRTV->GetDesc(&rtvDesc);
        LOG_CALL(LL_DBG, pDevice->CreateRenderTargetView(pLinearDepthTexture.Get(), &rtvDesc,
                                                         pCurrentRTV.ReleaseAndGetAddressOf()));
        LOG_CALL(LL_DBG, pThis->OMSetRenderTargets(1, pCurrentRTV.GetAddressOf(), NULL));

        D3D11_TEXTURE2D_DESC ldtDesc;
        pLinearDepthTexture->GetDesc(&ldtDesc);

        D3D11_VIEWPORT viewport;
        viewport.Width = static_cast<float>(ldtDesc.Width);
        viewport.Height = static_cast<float>(ldtDesc.Height);
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.MinDepth = 0;
        viewport.MaxDepth = 1;
        pThis->RSSetViewports(1, &viewport);
        D3D11_RECT rect;
        rect.left = rect.top = 0;
        rect.right = static_cast<LONG>(ldtDesc.Width);
        rect.bottom = static_cast<LONG>(ldtDesc.Height);
        pThis->RSSetScissorRects(1, &rect);

        LOG_CALL(LL_TRC, OriginalFunc(pThis, IndexCount, StartIndexLocation, BaseVertexLocation));
    }
}

HANDLE GameHooks::CreateThread::Implementation(void* pFunc, void* pParams, int32_t r8d, int32_t r9d, void* rsp20,
                                               int32_t rsp28, char* name) {
    PRE();
    void* result = GameHooks::CreateThread::OriginalFunc(pFunc, pParams, r8d, r9d, rsp20, rsp28, name);
    LOG(LL_TRC, "CreateThread:", " pFunc:", pFunc, " pParams:", pParams, " r8d:", Logger::hex(r8d, 4),
        " r9d:", Logger::hex(r9d, 4), " rsp20:", rsp20, " rsp28:", rsp28, " name ptr:",
        static_cast<const void*>(name), " name:", SafeAnsiString(name));
    POST();
    return result;
}

bool GameHooks::StartBakeProject::Implementation(void* videoEditorInterface, void* montage) {
    PRE();
    
    LOG(LL_NFO, "StartBakeProject called");
    LOG(LL_DBG, "  videoEditorInterface:", videoEditorInterface);
    LOG(LL_DBG, "  montage:", montage);

    if (dualPassContext &&
        (dualPassContext->state == DualPassState::PASS2_PENDING ||
         dualPassContext->state == DualPassState::PASS2_RUNNING)) {
        LOG(LL_NFO, "Internal Pass 2 StartBakeProject execution detected; preserving dual-pass state");
    } else {
        const float gameFrameRate =
            (static_cast<float>(Config::Manager::fps.first) *
             (static_cast<float>(Config::Manager::motion_blur_samples) + 1) /
             static_cast<float>(Config::Manager::fps.second));

        LOG(LL_NFO, "User-initiated export - effective frame rate: ", gameFrameRate, " FPS");

        if (gameFrameRate > 60.0f) {
            LOG(LL_NFO, "High frame rate detected - activating DUAL-PASS mode");

            if (!dualPassContext) {
                dualPassContext.reset(new DualPassContext());
            }

            dualPassContext->state = DualPassState::PASS1_RUNNING;
            dualPassContext->original_fps = Config::Manager::fps;
            dualPassContext->original_motion_blur_samples = Config::Manager::motion_blur_samples;
            dualPassContext->saved_video_editor_interface = videoEditorInterface;
            dualPassContext->saved_montage_ptr = montage;

            g_pass2TriggerIssued = false;
            g_pass2StartupPending = false;
            g_pass2StartupDelayFrames = 0;

            LOG(LL_NFO, "  PASS 1 will capture audio at 30 FPS, 0 motion blur");
            LOG(LL_NFO, "  PASS 2 will capture video at ", Config::Manager::fps.first, "/", Config::Manager::fps.second,
                " FPS, ", Config::Manager::motion_blur_samples, " motion blur samples");
            LOG(LL_DBG, "  Saved pointers - interface:", videoEditorInterface, " montage:", montage);
        } else {
            LOG(LL_DBG, "Normal frame rate - single-pass export");

            if (dualPassContext && dualPassContext->state != DualPassState::IDLE) {
                LOG(LL_WRN, "Dual-pass context was not IDLE - resetting");
                dualPassContext->state = DualPassState::IDLE;
                g_pass2TriggerIssued = false;
                g_pass2StartupPending = false;
                g_pass2StartupDelayFrames = 0;
            }
        }
    }
    
    bool result = GameHooks::StartBakeProject::OriginalFunc(videoEditorInterface, montage);
    LOG(LL_DBG, "StartBakeProject original returned:", result);
    POST();
    return result;
}

bool GameHooks::EnhancedStartBakeProject::Implementation(void* montage, const char* exportName) {
    PRE();

    LOG(LL_NFO, "EnhancedStartBakeProject called");
    LOG(LL_DBG, "  montage:", montage,
        " exportName:", exportName ? SafeAnsiString(exportName) : "<default>");

    if (dualPassContext &&
        (dualPassContext->state == DualPassState::PASS2_PENDING ||
         dualPassContext->state == DualPassState::PASS2_RUNNING)) {
        LOG(LL_NFO, "Internal Enhanced Pass 2 start detected; preserving dual-pass state");
    } else {
        const float gameFrameRate =
            (static_cast<float>(Config::Manager::fps.first) *
             (static_cast<float>(Config::Manager::motion_blur_samples) + 1.0f) /
             static_cast<float>(Config::Manager::fps.second));

        LOG(LL_NFO, "Enhanced user export - effective frame rate: ", gameFrameRate, " FPS");

        if (gameFrameRate > 60.0f && !isEnhancedDualPassSupported) {
            LOG(LL_ERR, "Enhanced high-rate export refused because the dual-pass state machine is unavailable; ",
                "EVER will not silently export without its required synchronized-audio workflow");
            POST();
            return false;
        }

        if (gameFrameRate > 60.0f) {
            LOG(LL_NFO, "Enhanced high-rate export - activating DUAL-PASS audio workflow");

            if (!dualPassContext) {
                dualPassContext.reset(new DualPassContext());
            }

            dualPassContext->state = DualPassState::PASS1_RUNNING;
            dualPassContext->original_fps = Config::Manager::fps;
            dualPassContext->original_motion_blur_samples = Config::Manager::motion_blur_samples;
            // Enhanced's first argument is the montage container. Keep it in the
            // historically named field so the shared pass-two scheduler can own it.
            dualPassContext->saved_video_editor_interface = montage;
            dualPassContext->saved_montage_ptr = nullptr;
            dualPassContext->enhanced_export_name_present = exportName != nullptr;
            dualPassContext->enhanced_export_name = exportName ? SafeAnsiString(exportName) : std::string();
            dualPassContext->audio_buffer.clear();
            dualPassContext->audio_buffer_playback_position = 0;

            g_pass2TriggerIssued = false;
            g_pass2StartupPending = false;
            g_pass2StartupDelayFrames = 0;

            LOG(LL_NFO, "  PASS 1: audio only at 30 FPS, 0 motion-blur samples");
            LOG(LL_NFO, "  PASS 2: video at ", Config::Manager::fps.first, "/",
                Config::Manager::fps.second, " FPS, ",
                Config::Manager::motion_blur_samples, " motion-blur samples");
        } else {
            LOG(LL_DBG, "Enhanced single-pass export selected");
            if (dualPassContext && dualPassContext->state != DualPassState::IDLE) {
                LOG(LL_WRN, "Resetting stale Enhanced dual-pass context before single-pass export");
                dualPassContext->state = DualPassState::IDLE;
                dualPassContext->audio_buffer.clear();
                dualPassContext->enhanced_export_name.clear();
                dualPassContext->enhanced_export_name_present = false;
                g_pass2TriggerIssued = false;
                g_pass2StartupPending = false;
                g_pass2StartupDelayFrames = 0;
            }
        }
    }

    const bool result = OriginalFunc(montage, exportName);
    LOG(LL_NFO, "EnhancedStartBakeProject original returned: ", result);
    POST();
    return result;
}

void GameHooks::EnhancedCleanupReplayPlayback::Implementation() {
    PRE();
    const bool pass1Handoff = dualPassContext &&
        dualPassContext->state == DualPassState::PASS1_COMPLETE;

    LOG(LL_NFO, "EnhancedCleanupReplayPlayback called. dualPass=",
        dualPassContext ? dualPassStateName(dualPassContext->state) : "NO_CONTEXT");

    if (pass1Handoff) {
        g_enhancedDualPassCleanupInProgress = true;
        const bool cleanupSucceeded = callEnhancedCleanupReplayPlaybackOriginal();
        g_enhancedDualPassCleanupInProgress = false;

        if (!cleanupSucceeded) {
            LOG(LL_ERR, "Enhanced Pass 1 cleanup failed; Pass 2 cannot be started safely");
            POST();
            return;
        }

        if (!g_pass2TriggerIssued.exchange(true)) {
            schedulePass2StartupOnMainThread("EnhancedCleanupReplayPlayback(pass1-complete)");
            LOG(LL_NFO, "Enhanced Pass 1 cleanup complete; Pass 2 scheduled on the main thread");
        } else {
            LOG(LL_DBG, "Enhanced Pass 2 trigger was already issued");
        }
        POST();
        return;
    }

    if (!callEnhancedCleanupReplayPlaybackOriginal()) {
        LOG(LL_ERR, "Enhanced normal cleanup failed");
        POST();
        return;
    }

    if (dualPassContext && dualPassContext->state == DualPassState::PASS2_COMPLETE) {
        LOG(LL_NFO, "Enhanced Pass 2 cleanup complete; resetting dual-pass context to IDLE");
        dualPassContext->state = DualPassState::IDLE;
        dualPassContext->enhanced_export_name.clear();
        dualPassContext->enhanced_export_name_present = false;
        g_pass2TriggerIssued = false;
        g_pass2StartupPending = false;
        g_pass2StartupDelayFrames = 0;
    }

    POST();
}

void GameHooks::EnhancedKillPlaybackOrBake::Implementation(bool userCancelled) {
    PRE();

    LOG(LL_NFO, "EnhancedKillPlaybackOrBake called. userCancelled=", userCancelled,
        " dualPass=", dualPassContext ? dualPassStateName(dualPassContext->state) : "NO_CONTEXT",
        " cleanupInProgress=", g_enhancedDualPassCleanupInProgress);

    if (dualPassContext &&
        (dualPassContext->state == DualPassState::PASS1_RUNNING ||
         dualPassContext->state == DualPassState::PASS2_RUNNING) &&
        !userCancelled && !g_enhancedDualPassCleanupInProgress) {
        if (!callEnhancedKillPlaybackOrBakeThroughFinalize()) {
            LOG(LL_ERR, "Enhanced partial KillPlaybackOrBake execution failed; preserving ",
                dualPassStateName(dualPassContext->state), " state");
        }
        POST();
        return;
    }

    if (dualPassContext && dualPassContext->state == DualPassState::PASS1_COMPLETE &&
        !userCancelled && !g_enhancedDualPassCleanupInProgress) {
        LOG(LL_NFO, "Suppressing duplicate Enhanced KillPlaybackOrBake while Pass 1 awaits cleanup");
        POST();
        return;
    }

    if (!callEnhancedKillPlaybackOrBakeOriginal(userCancelled)) {
        LOG(LL_ERR, "Enhanced KillPlaybackOrBake original call failed");
        POST();
        return;
    }

    if (userCancelled && !g_enhancedDualPassCleanupInProgress && dualPassContext &&
        dualPassContext->state != DualPassState::PASS2_COMPLETE) {
        LOG(LL_NFO, "Enhanced dual-pass export cancelled; resetting buffered audio state");
        dualPassContext->state = DualPassState::IDLE;
        dualPassContext->audio_buffer.clear();
        dualPassContext->enhanced_export_name.clear();
        dualPassContext->enhanced_export_name_present = false;
        g_pass2TriggerIssued = false;
        g_pass2StartupPending = false;
        g_pass2StartupDelayFrames = 0;
    }

    POST();
}

bool GameHooks::HasVideoRenderErrored::Implementation() {
    PRE();
    const bool originalResult = OriginalFunc();

    if (asyncFinalizeInProgress.load()) {
        // During async trailer flush, keep Rockstar in non-error transition state.
        POST();
        return false;
    }

    if (asyncFinalizeCompleted.load() && asyncFinalizeFailed.load()) {
        LOG(LL_ERR, "Reporting render error after async FFmpeg finalization failure.");
        POST();
        return true;
    }

    if (dualPassContext &&
        (dualPassContext->state == DualPassState::PASS1_RUNNING ||
         dualPassContext->state == DualPassState::PASS1_COMPLETE ||
         dualPassContext->state == DualPassState::PASS2_PENDING)) {
        if (originalResult) {
            LOG(LL_NFO, "Suppressing HasVideoRenderErrored=true during dual-pass handoff. state=",
                dualPassStateName(dualPassContext->state));
        }
        POST();
        return false;
    }

    POST();
    return originalResult;
}

bool GameHooks::ShouldShowLoadingScreen::Implementation() {
    PRE();
    const bool originalResult = OriginalFunc();

    if (asyncFinalizeInProgress.load()) {
        // Keep Playback::CheckExportStatus in loading path until async finalize finishes.
        POST();
        return true;
    }

    if (dualPassContext &&
        (dualPassContext->state == DualPassState::PASS1_COMPLETE ||
         dualPassContext->state == DualPassState::PASS2_PENDING)) {
        if (dualPassContext->state == DualPassState::PASS1_COMPLETE &&
            !g_pass2TriggerIssued.load() &&
            !g_pass2StartupPending.load() &&
            pCleanupReplayPlaybackOriginal != nullptr &&
            std::this_thread::get_id() == mainThreadId) {
            LOG(LL_NFO, "ShouldShowLoadingScreen: PASS1_COMPLETE handoff trigger via main-thread fallback");
            try {
                reinterpret_cast<CleanupReplayPlaybackInternalFunc>(pCleanupReplayPlaybackOriginal)();
            } catch (const std::exception& ex) {
                LOG(LL_ERR, "ShouldShowLoadingScreen fallback handoff exception: ", ex.what());
                g_pass2TriggerIssued = false;
            } catch (...) {
                LOG(LL_ERR, "ShouldShowLoadingScreen fallback handoff unknown exception");
                g_pass2TriggerIssued = false;
            }
        }

        if (dualPassContext->state == DualPassState::PASS2_PENDING &&
            g_pass2StartupPending.load() &&
            std::this_thread::get_id() == mainThreadId) {
            LOG(LL_NFO, "ShouldShowLoadingScreen: firing Pass 2 startup on main thread (WAIT(0) blocked in loading-screen loop)");
            executePass2StartupOnMainThread();
        }

        if (!originalResult) {
            LOG(LL_DBG, "Forcing loading screen ON during dual-pass handoff. state=",
                dualPassStateName(dualPassContext->state));
        }
        POST();
        return true;
    }

    POST();
    return originalResult;
}

void GameHooks::PuddlesRipplesUpdate::Implementation(void* ripples, float rainyness) {
    PRE();

    const bool exportActive = ever::isExportActive();
    if (!exportActive) {
        {
            std::lock_guard<std::mutex> lock(g_temporalSyncMutex);
            updateTemporalSyncExportTransitionLocked(false);
        }
        OriginalFunc(ripples, rainyness);
        POST();
        return;
    }

    if (!g_loggedRipplesExportHit.exchange(true)) {
        LOG(LL_NFO, "PuddlesRipplesUpdate hit during export. ripples=",
            Logger::hex(reinterpret_cast<uint64_t>(ripples), 16),
            " rainyness=", rainyness);
    }

    const double stepSec = getCurrentExportRenderStepSeconds();
    bool shouldRun = true;
    uint64_t calls = 0;
    uint64_t skipped = 0;

    {
        std::lock_guard<std::mutex> lock(g_temporalSyncMutex);
        updateTemporalSyncExportTransitionLocked(true);

        ++g_temporalSyncState.ripplesCalls;
        shouldRun = shouldExecuteTemporalTick(g_temporalSyncState.ripplesAccumulatorSec, stepSec);
        if (!shouldRun) {
            ++g_temporalSyncState.ripplesSkipped;
        }

        calls = g_temporalSyncState.ripplesCalls;
        skipped = g_temporalSyncState.ripplesSkipped;
        logTemporalSyncCounters("ripples", calls, skipped, stepSec);
    }

    if (!shouldRun) {
        POST();
        return;
    }

    OriginalFunc(ripples, rainyness);
    POST();
}

void GameHooks::ClothManagerUpdate::Implementation(void* thisPtr, int typeToUpdate) {
    PRE();

    if (!ever::isExportActive() || !g_pClothSystemTimeStep) {
        OriginalFunc(thisPtr, typeToUpdate);
        POST();
        return;
    }

    const float exportStep = static_cast<float>(getCurrentExportRenderStepSeconds());
    const float savedSystemStep = *g_pClothSystemTimeStep;
    *g_pClothSystemTimeStep = exportStep;

    if (!g_loggedClothExportHit.exchange(true)) {
        LOG(LL_NFO, "ClothManagerUpdate: patching sm_systemTime.m_StepInSeconds ",
            savedSystemStep * 1000.0f, "ms -> ", exportStep * 1000.0f, "ms",
            " typeToUpdate=", typeToUpdate);
    }

    OriginalFunc(thisPtr, typeToUpdate);
    *g_pClothSystemTimeStep = savedSystemStep;
    POST();
}

float GameHooks::GetRenderTimeBase::Implementation(int64_t choice) {
    PRE();
    const std::pair<int32_t, int32_t> fps = Config::Manager::fps;
    const float result = 1000.0f * static_cast<float>(fps.second) /
                         (static_cast<float>(fps.first) * (static_cast<float>(Config::Manager::motion_blur_samples) + 1));
    const uint64_t logKey =
        (static_cast<uint64_t>(static_cast<uint32_t>(fps.first)) << 32) ^
        static_cast<uint32_t>(fps.second) ^
        (static_cast<uint64_t>(Config::Manager::motion_blur_samples) << 24);
    static std::atomic_uint64_t lastLoggedKey = std::numeric_limits<uint64_t>::max();
    if (lastLoggedKey.exchange(logKey, std::memory_order_relaxed) != logKey) {
        LOG(LL_NFO, "Export render time step: ", result, " ms (fps=", fps.first, "/", fps.second,
            " motionBlurSamples=", static_cast<uint32_t>(Config::Manager::motion_blur_samples), ")");
    }
    POST();
    return result;
}

void AutoReloadConfig() {
    if (Config::Manager::auto_reload_config) {
        LOG_CALL(LL_DBG, Config::Manager::reload());
    }
}

void CreateNewExportContext() {
    encodingSession.reset(new Encoder::EncoderSession());
    NOT_NULL(encodingSession, "Could not create the encoding session");
    ::exportContext.reset(new ExportContext());
    NOT_NULL(::exportContext, "Could not create export context");
    hasLoggedEnhancedCaptureResolutionMismatch = false;
    hasLoggedEnhancedCaptureSuccess = false;
    hasLoggedEnhancedNamedCaptureFailure = false;
    resetEnhancedCaptureTracking();
    clearFfmpegExportFailure();
    clearAsyncFinalizeState();
    
    // Handle dual-pass overrides - must be set AFTER config reload
    LOG(LL_NFO, "CreateNewExportContext called");
    if (dualPassContext) {
        LOG(LL_NFO, "  dualPassContext exists, state =", static_cast<int>(dualPassContext->state));
        if (dualPassContext->state == DualPassState::PASS1_RUNNING) {
            // Override config for Pass 1: 30 FPS, 0 motion blur
            LOG(LL_NFO, "  PASS 1 DETECTED - Overriding config for audio capture");
            LOG(LL_NFO, "    Before: fps=", Config::Manager::fps.first, "/", Config::Manager::fps.second, 
                        " motion_blur=", Config::Manager::motion_blur_samples);
            Config::Manager::fps = std::make_pair(30, 1);
            Config::Manager::motion_blur_samples = 0;
            LOG(LL_NFO, "    After: fps=30/1 motion_blur=0");
            
            ::exportContext->is_audio_export_disabled = false;
            LOG(LL_NFO, "  Audio ENABLED for Pass 1");
        } else if (dualPassContext->state == DualPassState::PASS2_PENDING || 
                   dualPassContext->state == DualPassState::PASS2_RUNNING) {
            // Restore original user settings for Pass 2 (video capture)
            LOG(LL_NFO, "  PASS 2 DETECTED - Restoring user settings for video capture");
            LOG(LL_NFO, "    Restoring: fps=", dualPassContext->original_fps.first, "/", 
                        dualPassContext->original_fps.second, " motion_blur=", 
                        dualPassContext->original_motion_blur_samples);
            Config::Manager::fps = dualPassContext->original_fps;
            Config::Manager::motion_blur_samples = dualPassContext->original_motion_blur_samples;
            
            ::exportContext->is_audio_export_disabled = false;
            LOG(LL_NFO, "  Audio ENABLED for Pass 2 (replaying buffered audio)");
        } else {
            LOG(LL_NFO, "  State is neither PASS1 nor PASS2");
        }
    } else {
        LOG(LL_NFO, "  dualPassContext is NULL");
    }
}

void* GameHooks::CreateTexture::Implementation(void* rcx, char* name, const uint32_t r8d, uint32_t width,
                                               uint32_t height, const uint32_t format, void* rsp30) {
    const std::string safeName = SafeAnsiString(name);
    const bool trackedTexture = IsTrackedCreateTextureName(safeName);
    const bool runningInFiveM = IsRunningInFiveMRuntime();

    static std::atomic<uint64_t> s_createTextureCalls{0};
    const uint64_t callIndex = s_createTextureCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    if (trackedTexture) {
        LOG(LL_DBG, "CreateTexture tracked entry #", callIndex,
            " rcx:", rcx,
            " name:", safeName,
            " name ptr:", static_cast<const void*>(name),
            " width:", width,
            " height:", height,
            " format:", Logger::hex(format, 8));
    }

    void* result = OriginalFunc(rcx, name, r8d, width, height, format, rsp30);

    ComPtr<ID3D11Texture2D> pTexture;
    DXGI_FORMAT fmt = DXGI_FORMAT::DXGI_FORMAT_UNKNOWN;

    auto canRead = [](const void* ptr, size_t bytes) -> bool {
        return ptr != nullptr && IsBadReadPtr(ptr, bytes) == 0;
    };

    auto tryQueryTexture = [&](IUnknown* pUnknown) -> bool {
        if (!canRead(pUnknown, sizeof(void*))) {
            return false;
        }

        // Validate the vtable pointer before issuing a COM virtual call.
        void** vtbl = *reinterpret_cast<void***>(pUnknown);
        if (!canRead(vtbl, sizeof(void*)) || !canRead(vtbl[0], sizeof(void*))) {
            return false;
        }

        return SUCCEEDED(pUnknown->QueryInterface(pTexture.GetAddressOf()));
    };

    if (result) {
        bool queried = false;

        if (runningInFiveM) {
            // FiveM may return an internal wrapper here; keep the legacy slot-7 path.
            IUnknown* slot7Unknown = nullptr;
            if (canRead(result, sizeof(void*) * 8)) {
                slot7Unknown = *(reinterpret_cast<IUnknown**>(result) + 7);
            }
            queried = tryQueryTexture(slot7Unknown);
        } else {
            // Regular GTA V: probe direct COM result first, then legacy slot-7 fallback.
            queried = tryQueryTexture(static_cast<IUnknown*>(result));

            if (!queried) {
                IUnknown* slot7Unknown = nullptr;
                if (canRead(result, sizeof(void*) * 8)) {
                    slot7Unknown = *(reinterpret_cast<IUnknown**>(result) + 7);
                }
                queried = tryQueryTexture(slot7Unknown);
            }
        }

        if (queried && pTexture) {
            D3D11_TEXTURE2D_DESC desc;
            pTexture->GetDesc(&desc);
            fmt = desc.Format;
        }
    }

    const bool suspiciousNamePtr = safeName.rfind("<INVALID PTR", 0) == 0;
    if (trackedTexture || suspiciousNamePtr) {
        LOG(LL_DBG, "CreateTexture result #", callIndex,
            " name:", safeName,
            " result:", result,
            " fmt:", conv_dxgi_format_to_string(fmt),
            " queriedTexture:", (pTexture ? "yes" : "no"));
    } else if ((callIndex % 5000) == 0) {
        LOG(LL_DBG, "CreateTexture heartbeat #", callIndex,
            " name:", safeName,
            " result:", result,
            " fmt:", conv_dxgi_format_to_string(fmt));
    }

    static bool presentHooked = false;
    if (!presentHooked && pTexture && !mainSwapChain) {
        presentHooked = true;
    }

    if (pTexture) {
        ComPtr<ID3D11Device> pDevice;
        pTexture->GetDevice(pDevice.GetAddressOf());
        if ((safeName == "DepthBuffer_Resolved") || (safeName == "DepthBufferCopy")) {
            pGameDepthBufferResolved = pTexture;
        } else if (safeName == "DepthBuffer") {
            pGameDepthBuffer = pTexture;
        } else if (safeName == "Depth Quarter") {
            pGameDepthBufferQuarter = pTexture;
        } else if (safeName == "GBUFFER_0") {
            pGameGBuffer0 = pTexture;
        } else if (safeName == "Edge Copy") {
            pGameEdgeCopy = pTexture;
            D3D11_TEXTURE2D_DESC desc;
            pTexture->GetDesc(&desc);
            REQUIRE(pDevice->CreateTexture2D(&desc, nullptr, pStencilTexture.GetAddressOf()),
                    "Failed to create stencil texture");
        } else if (safeName == "Depth Quarter Linear") {
            pGameDepthBufferQuarterLinear = pTexture;
            D3D11_TEXTURE2D_DESC desc, resolvedDesc;

            if (!pGameDepthBufferResolved) {
                pGameDepthBufferResolved = pGameDepthBuffer;
            }

            pGameDepthBufferResolved->GetDesc(&resolvedDesc);
            resolvedDesc.ArraySize = 1;
            resolvedDesc.SampleDesc.Count = 1;
            resolvedDesc.SampleDesc.Quality = 0;
            pGameDepthBufferQuarterLinear->GetDesc(&desc);
            desc.Width = resolvedDesc.Width;
            desc.Height = resolvedDesc.Height;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            LOG_CALL(LL_DBG, pDevice->CreateTexture2D(&desc, NULL, pLinearDepthTexture.GetAddressOf()));
        } else if (safeName == "BackBuffer") {
            pGameBackBufferResolved = nullptr;
            pGameDepthBuffer = nullptr;
            pGameDepthBufferQuarter = nullptr;
            pGameDepthBufferQuarterLinear = nullptr;
            pGameDepthBufferResolved = nullptr;
            pGameEdgeCopy = nullptr;
            pGameGBuffer0 = nullptr;

            pGameBackBuffer = pTexture;

        } else if ((safeName == "BackBuffer_Resolved") || (safeName == "BackBufferCopy")) {
            pGameBackBufferResolved = pTexture;
        } else if (safeName == "VideoEncode") {
            std::lock_guard<std::mutex> sessionLock(mxSession);
            const ComPtr<ID3D11Texture2D>& pExportTexture = pTexture;

            D3D11_TEXTURE2D_DESC desc;
            pExportTexture->GetDesc(&desc);

            LOG(LL_DBG, "CreateTexture VideoEncode acquired. width=", desc.Width,
                " height=", desc.Height,
                " format=", conv_dxgi_format_to_string(desc.Format));

            LOG_CALL(LL_DBG, ::exportContext.reset());
            LOG_CALL(LL_DBG, encodingSession.reset());
            try {
                LOG(LL_NFO, "Creating session...");

                AutoReloadConfig();
                CreateNewExportContext();
                const bool swapChainSignalReceived = waitForMainSwapChain(kSwapChainReadyTimeout);
                if (!swapChainSignalReceived) {
                    LOG(LL_DBG, "Timed out waiting for main swap chain after VideoEncode texture creation; will retry on bind");
                }

                if (!bindExportSwapChainIfAvailable()) {
                    LOG(LL_WRN, "Main swap chain not ready when VideoEncode texture was created; binding will be retried");
                }
                ::exportContext->p_export_render_target = pExportTexture;

                pExportTexture->GetDevice(::exportContext->p_device.GetAddressOf());
                ::exportContext->p_device->GetImmediateContext(::exportContext->p_device_context.GetAddressOf());
            } catch (std::exception& ex) {
                LOG(LL_ERR, ex.what());
                LOG_CALL(LL_DBG, encodingSession.reset());
                LOG_CALL(LL_DBG, ::exportContext.reset());
            }
        }
    } else if (trackedTexture) {
        LOG(LL_WRN, "CreateTexture tracked texture without ID3D11Texture2D result. name=", safeName,
            " rcx=", rcx,
            " result=", result,
            " width=", width,
            " height=", height,
            " format=", Logger::hex(format, 8));
    }

    return result;
}

void ever::prepareDeferredContext(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext) {
    REQUIRE(pDevice->CreateDeferredContext(0, pDContext.GetAddressOf()), "Failed to create deferred context");
    REQUIRE(pDevice->CreateVertexShader(VSFullScreen::g_main, sizeof(VSFullScreen::g_main), NULL,
                                        pVsFullScreen.GetAddressOf()),
            "Failed to create g_VSFullScreen vertex shader");
    REQUIRE(pDevice->CreatePixelShader(PSAccumulate::g_main, sizeof(PSAccumulate::g_main), NULL,
                                       pPsAccumulate.GetAddressOf()),
            "Failed to create pixel shader");
    REQUIRE(pDevice->CreatePixelShader(PSDivide::g_main, sizeof(PSDivide::g_main), NULL, pPsDivide.GetAddressOf()),
            "Failed to create pixel shader");

    D3D11_BUFFER_DESC clipBufDesc;
    clipBufDesc.BindFlags = D3D11_BIND_FLAG::D3D11_BIND_CONSTANT_BUFFER;
    clipBufDesc.ByteWidth = sizeof(XMFLOAT4);
    clipBufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_FLAG::D3D11_CPU_ACCESS_WRITE;
    clipBufDesc.MiscFlags = 0;
    clipBufDesc.StructureByteStride = 0;
    clipBufDesc.Usage = D3D11_USAGE::D3D11_USAGE_DYNAMIC;

    REQUIRE(pDevice->CreateBuffer(&clipBufDesc, NULL, pDivideConstantBuffer.GetAddressOf()),
            "Failed to create constant buffer for pixel shader");

    D3D11_SAMPLER_DESC samplerDesc;
    samplerDesc.Filter = D3D11_FILTER::D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_MODE::D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_MODE::D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_MODE::D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_FUNC::D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    REQUIRE(pDevice->CreateSamplerState(&samplerDesc, pPsSampler.GetAddressOf()), "Failed to create sampler state");

    D3D11_RASTERIZER_DESC rasterStateDesc;
    rasterStateDesc.AntialiasedLineEnable = FALSE;
    rasterStateDesc.CullMode = D3D11_CULL_FRONT;
    rasterStateDesc.DepthClipEnable = FALSE;
    rasterStateDesc.FillMode = D3D11_FILL_MODE::D3D11_FILL_SOLID;
    rasterStateDesc.MultisampleEnable = FALSE;
    rasterStateDesc.ScissorEnable = FALSE;
    REQUIRE(pDevice->CreateRasterizerState(&rasterStateDesc, pRasterState.GetAddressOf()),
            "Failed to create raster state");

    D3D11_BLEND_DESC blendDesc;
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    REQUIRE(pDevice->CreateBlendState(&blendDesc, pAccBlendState.GetAddressOf()),
            "Failed to create accumulation blend state");
}

void ever::drawAdditive(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                       ComPtr<ID3D11Texture2D> pSource) {
    D3D11_TEXTURE2D_DESC desc;
    pSource->GetDesc(&desc);

    LOG_CALL(LL_DBG, pDContext->ClearState());
    LOG_CALL(LL_DBG, pDContext->IASetIndexBuffer(NULL, DXGI_FORMAT::DXGI_FORMAT_UNKNOWN, 0));
    LOG_CALL(LL_DBG, pDContext->IASetInputLayout(NULL));
    LOG_CALL(LL_DBG, pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP));

    LOG_CALL(LL_DBG, pDContext->VSSetShader(pVsFullScreen.Get(), NULL, 0));
    LOG_CALL(LL_DBG, pDContext->PSSetSamplers(0, 1, pPsSampler.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->PSSetShader(pPsAccumulate.Get(), NULL, 0));
    LOG_CALL(LL_DBG, pDContext->RSSetState(pRasterState.Get()));

    float factors[4] = {0, 0, 0, 0};

    LOG_CALL(LL_DBG, pDContext->OMSetBlendState(pAccBlendState.Get(), factors, 0xFFFFFFFF));

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;
    LOG_CALL(LL_DBG, pDContext->RSSetViewports(1, &viewport));

    LOG_CALL(LL_DBG, pDContext->CopyResource(pSourceSrvTexture.Get(), pSource.Get()));

    LOG_CALL(LL_DBG, pDContext->PSSetShaderResources(0, 1, pSourceSrv.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->OMSetRenderTargets(1, pRtvAccBuffer.GetAddressOf(), nullptr));
    if (::exportContext->acc_count == 0) {
        float color[4] = {0, 0, 0, 1};
        LOG_CALL(LL_DBG, pDContext->ClearRenderTargetView(pRtvAccBuffer.Get(), color));
    }

    LOG_CALL(LL_DBG, pDContext->Draw(4, 0));

    ComPtr<ID3D11CommandList> pCmdList;
    LOG_CALL(LL_DBG, pDContext->FinishCommandList(FALSE, pCmdList.GetAddressOf()));
    LOG_CALL(LL_DBG, pContext->ExecuteCommandList(pCmdList.Get(), TRUE));

    ::exportContext->acc_count++;
}

ComPtr<ID3D11Texture2D> ever::divideBuffer(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                                          uint32_t k) {

    D3D11_TEXTURE2D_DESC desc;
    pMotionBlurFinalBuffer->GetDesc(&desc);

    LOG_CALL(LL_DBG, pDContext->ClearState());
    LOG_CALL(LL_DBG, pDContext->IASetIndexBuffer(NULL, DXGI_FORMAT::DXGI_FORMAT_UNKNOWN, 0));
    LOG_CALL(LL_DBG, pDContext->IASetInputLayout(NULL));
    LOG_CALL(LL_DBG, pDContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP));

    LOG_CALL(LL_DBG, pDContext->VSSetShader(pVsFullScreen.Get(), NULL, 0));
    LOG_CALL(LL_DBG, pDContext->PSSetShader(pPsDivide.Get(), NULL, 0));

    D3D11_MAPPED_SUBRESOURCE mapped;
    REQUIRE(pDContext->Map(pDivideConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Failed to map divide shader constant buffer");
    ;
    const auto floats = static_cast<float*>(mapped.pData);
    floats[0] = static_cast<float>(k);
    /*floats[0] = 1.0f;*/
    LOG_CALL(LL_DBG, pDContext->Unmap(pDivideConstantBuffer.Get(), 0));

    LOG_CALL(LL_DBG, pDContext->PSSetConstantBuffers(0, 1, pDivideConstantBuffer.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->PSSetShaderResources(0, 1, pSrvAccBuffer.GetAddressOf()));
    LOG_CALL(LL_DBG, pDContext->RSSetState(pRasterState.Get()));

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;
    LOG_CALL(LL_DBG, pDContext->RSSetViewports(1, &viewport));

    LOG_CALL(LL_DBG, pDContext->OMSetRenderTargets(1, pRtvBlurBuffer.GetAddressOf(), NULL));
    constexpr float color[4] = {0, 0, 0, 1};
    LOG_CALL(LL_DBG, pDContext->ClearRenderTargetView(pRtvBlurBuffer.Get(), color));

    LOG_CALL(LL_DBG, pDContext->Draw(4, 0));

    ComPtr<ID3D11CommandList> pCmdList;
    LOG_CALL(LL_DBG, pDContext->FinishCommandList(FALSE, pCmdList.GetAddressOf()));
    LOG_CALL(LL_DBG, pContext->ExecuteCommandList(pCmdList.Get(), TRUE));

    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;

    ComPtr<ID3D11Texture2D> ret;
    REQUIRE(pDevice->CreateTexture2D(&desc, NULL, ret.GetAddressOf()),
            "Failed to create copy of motion blur dest texture");
    LOG_CALL(LL_DBG, pContext->CopyResource(ret.Get(), pMotionBlurFinalBuffer.Get()));

    return ret;
}

bool installAbsoluteJumpHook(uint64_t address, uintptr_t hookAddress) {
    if (address == 0 || hookAddress == 0) {
        return false;
    }

    constexpr size_t patchSize = 12; // mov rax, imm64; jmp rax
    std::array<uint8_t, patchSize> patch{};
    patch[0] = 0x48;
    patch[1] = 0xB8;

    std::memcpy(&patch[2], &hookAddress, sizeof(hookAddress));
    patch[10] = 0xFF;
    patch[11] = 0xE0;

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        const auto err = GetLastError();
        LOG(LL_ERR, "VirtualProtect failed for", Logger::hex(address, 16), ": ", err, " ", Logger::hex(err, 8));
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(address), patch.data(), patchSize);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), patchSize);

    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), patchSize, oldProtect, &unused);
    LOG(LL_DBG, "Installed absolute jump hook at", Logger::hex(address, 16), " -> ",
        Logger::hex(static_cast<uint64_t>(hookAddress), 16));
    return true;
}
