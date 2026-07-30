#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <dxgi.h>
#include <future>
#include <queue>
#include <reshade_api.hpp>
#include <wrl/client.h>

using namespace Microsoft::WRL;
    template <class T> void SafeRelease(T** ppT) {
        if (*ppT) {
            (*ppT)->Release();
            *ppT = NULL;
        }
    }

    template <class B, class A> B ForceCast(A a) {
        union {
            A a;
            B b;
        } x;
        x.a = a;
        return x.b;
    }

    namespace ever {
    void initialize();
    void ScriptMain();
    void OnPresent(IDXGISwapChain* p_swap_chain);
    void OnReshadeInitRuntime(reshade::api::effect_runtime* runtime);
    void OnReshadeDestroyRuntime(reshade::api::effect_runtime* runtime);
    void OnReshadeBeginEffects(reshade::api::effect_runtime* runtime,
                               reshade::api::command_list* commandList,
                               reshade::api::resource_view targetView);
    void OnReshadePresent(reshade::api::effect_runtime* runtime);
    void OnReshadeRenderTarget(reshade::api::device* device, reshade::api::resource_view view);
    void OnReshadeDepthStencil(reshade::api::device* device, reshade::api::resource_view view);
    void OnReshadeBarrier(reshade::api::device* device, uint32_t count,
                          const reshade::api::resource* resources,
                          const reshade::api::resource_usage* newStates);
    void OnReshadeCopyResource(reshade::api::device* device, reshade::api::resource source,
                               reshade::api::resource destination, const char* operation);
    void OnReshadeDestroyResource(reshade::api::resource resource);
    void finalize();
    bool isExportActive();
    bool isEnhancedEdition();
    bool isCustomFrameRateAvailable();
    bool isDualPassAvailable();
    static void prepareDeferredContext(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext);
    static ComPtr<ID3D11Texture2D> divideBuffer(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                                                uint32_t k);
    static void drawAdditive(ComPtr<ID3D11Device> pDevice, ComPtr<ID3D11DeviceContext> pContext,
                            ComPtr<ID3D11Texture2D> pSource);
}
