#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 26812)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#undef min
#undef max

#include "OpenEXRExporter.h"
#include "logger.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfRgba.h>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfThreading.h>
#include <ppl.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#pragma warning(pop)

namespace Encoder {
    void OpenEXRExporter::initialize(const std::string& outputPath, int32_t width, int32_t height) {
        PRE();

        static std::once_flag threadPoolInitialized;
        std::call_once(threadPoolInitialized, [] {
            const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
            const int workerThreads = static_cast<int>(std::clamp(
                hardwareThreads > 2 ? hardwareThreads - 2 : 1u, 1u, 8u));
            Imf::setGlobalThreadCount(workerThreads);
            LOG(LL_NFO, "OpenEXR global compression pool initialized. workers=", workerThreads,
                " hardwareThreads=", hardwareThreads);
        });
        
        outputPath_ = outputPath;
        width_ = width;
        height_ = height;
        
        std::filesystem::create_directories(outputPath_);
        
        LOG(LL_NFO, "OpenEXR exporter initialized: ", outputPath_, " (", width_, "x", height_, ")");
        
        POST();
    }

    HRESULT OpenEXRExporter::exportFrame(const Microsoft::WRL::ComPtr<ID3D11DeviceContext>& deviceContext,
                                        const Microsoft::WRL::ComPtr<ID3D11Texture2D>& colorTexture,
                                        const Microsoft::WRL::ComPtr<ID3D11Texture2D>& depthTexture,
                                        uint64_t frameNumber) {
        PRE();

        D3D11_MAPPED_SUBRESOURCE mappedColor = {nullptr};
        D3D11_MAPPED_SUBRESOURCE mappedDepth = {nullptr};

        if (colorTexture) {
            REQUIRE(deviceContext->Map(colorTexture.Get(), 0, D3D11_MAP::D3D11_MAP_READ, 0, &mappedColor), 
                    "Failed to map color texture for EXR export");
        }
        if (depthTexture) {
            REQUIRE(deviceContext->Map(depthTexture.Get(), 0, D3D11_MAP::D3D11_MAP_READ, 0, &mappedDepth),
                    "Failed to map depth texture for EXR export");
        }

        struct RGBA {
            half r;
            half g;
            half b;
            half a;
        };

        struct Depth {
            float depth;
        };

        Imf::Header header(width_, height_);
        Imf::FrameBuffer framebuffer;

        if (mappedColor.pData != nullptr) {
            LOG_CALL(LL_DBG, header.channels().insert("R", Imf::Channel(Imf::HALF)));
            LOG_CALL(LL_DBG, header.channels().insert("G", Imf::Channel(Imf::HALF)));
            LOG_CALL(LL_DBG, header.channels().insert("B", Imf::Channel(Imf::HALF)));
            LOG_CALL(LL_DBG, header.channels().insert("SubsurfaceScatter", Imf::Channel(Imf::HALF)));
            
            const auto colorArray = static_cast<RGBA*>(mappedColor.pData);

            LOG_CALL(LL_DBG, framebuffer.insert("R", Imf::Slice(Imf::HALF, 
                reinterpret_cast<char*>(&colorArray[0].r),
                sizeof(RGBA), sizeof(RGBA) * width_)));

            LOG_CALL(LL_DBG, framebuffer.insert("G", Imf::Slice(Imf::HALF, 
                reinterpret_cast<char*>(&colorArray[0].g),
                sizeof(RGBA), sizeof(RGBA) * width_)));

            LOG_CALL(LL_DBG, framebuffer.insert("B", Imf::Slice(Imf::HALF, 
                reinterpret_cast<char*>(&colorArray[0].b),
                sizeof(RGBA), sizeof(RGBA) * width_)));

            LOG_CALL(LL_DBG, framebuffer.insert("SubsurfaceScatter", Imf::Slice(Imf::HALF, 
                reinterpret_cast<char*>(&colorArray[0].a),
                sizeof(RGBA), sizeof(RGBA) * width_)));
        }

        if (mappedDepth.pData != nullptr) {
            LOG_CALL(LL_DBG, header.channels().insert("depth.Z", Imf::Channel(Imf::FLOAT)));
            
            const auto depthArray = static_cast<Depth*>(mappedDepth.pData);

            LOG_CALL(LL_DBG, framebuffer.insert("depth.Z", Imf::Slice(Imf::FLOAT, 
                reinterpret_cast<char*>(&depthArray[0].depth),
                sizeof(Depth), sizeof(Depth) * width_)));
        }

        std::stringstream filenameStream;
        filenameStream << outputPath_ << "\\frame." 
                    << std::setw(5) << std::setfill('0') << frameNumber 
                    << ".exr";

        Imf::OutputFile file(filenameStream.str().c_str(), header);
        LOG_CALL(LL_DBG, file.setFrameBuffer(framebuffer));
        LOG_CALL(LL_DBG, file.writePixels(height_));

        if (colorTexture) {
            LOG_CALL(LL_DBG, deviceContext->Unmap(colorTexture.Get(), 0));
        }

        if (depthTexture) {
            LOG_CALL(LL_DBG, deviceContext->Unmap(depthTexture.Get(), 0));
        }

        LOG(LL_NFO, "Exported EXR frame: ", frameNumber);

        POST();
        return S_OK;
    }

    HRESULT OpenEXRExporter::exportFrame(const uint8_t* rgbaData, uint32_t colorRowPitch,
                                         const float* depthData, uint32_t depthRowPitch,
                                         uint64_t frameNumber) {
        PRE();

        if ((rgbaData == nullptr && depthData == nullptr) || width_ <= 0 || height_ <= 0) {
            LOG(LL_ERR, "OpenEXR raw export received no channels or invalid dimensions");
            POST();
            return E_INVALIDARG;
        }
        if (rgbaData != nullptr && colorRowPitch < static_cast<uint32_t>(width_) * 4) {
            LOG(LL_ERR, "OpenEXR raw color row pitch is too small");
            POST();
            return E_INVALIDARG;
        }
        if (depthData != nullptr && depthRowPitch < static_cast<uint32_t>(width_) * sizeof(float)) {
            LOG(LL_ERR, "OpenEXR raw depth row pitch is too small");
            POST();
            return E_INVALIDARG;
        }

        struct RGBA {
            half r;
            half g;
            half b;
            half a;
        };

        std::vector<RGBA> colorPixels;
        std::vector<float> depthPixels;
        Imf::Header header(width_, height_);
        Imf::FrameBuffer framebuffer;

        if (rgbaData != nullptr) {
            colorPixels.resize(static_cast<size_t>(width_) * height_);
            Concurrency::parallel_for<int32_t>(0, height_, [&](const int32_t y) {
                const uint8_t* sourceRow = rgbaData + static_cast<size_t>(y) * colorRowPitch;
                RGBA* destinationRow = colorPixels.data() + static_cast<size_t>(y) * width_;
                for (int32_t x = 0; x < width_; ++x) {
                    destinationRow[x].r = static_cast<float>(sourceRow[x * 4 + 0]) / 255.0f;
                    destinationRow[x].g = static_cast<float>(sourceRow[x * 4 + 1]) / 255.0f;
                    destinationRow[x].b = static_cast<float>(sourceRow[x * 4 + 2]) / 255.0f;
                    destinationRow[x].a = static_cast<float>(sourceRow[x * 4 + 3]) / 255.0f;
                }
            });

            header.channels().insert("R", Imf::Channel(Imf::HALF));
            header.channels().insert("G", Imf::Channel(Imf::HALF));
            header.channels().insert("B", Imf::Channel(Imf::HALF));
            header.channels().insert("SubsurfaceScatter", Imf::Channel(Imf::HALF));
            framebuffer.insert("R", Imf::Slice(Imf::HALF,
                reinterpret_cast<char*>(&colorPixels[0].r), sizeof(RGBA), sizeof(RGBA) * width_));
            framebuffer.insert("G", Imf::Slice(Imf::HALF,
                reinterpret_cast<char*>(&colorPixels[0].g), sizeof(RGBA), sizeof(RGBA) * width_));
            framebuffer.insert("B", Imf::Slice(Imf::HALF,
                reinterpret_cast<char*>(&colorPixels[0].b), sizeof(RGBA), sizeof(RGBA) * width_));
            framebuffer.insert("SubsurfaceScatter", Imf::Slice(Imf::HALF,
                reinterpret_cast<char*>(&colorPixels[0].a), sizeof(RGBA), sizeof(RGBA) * width_));
        }

        if (depthData != nullptr) {
            depthPixels.resize(static_cast<size_t>(width_) * height_);
            Concurrency::parallel_for<int32_t>(0, height_, [&](const int32_t y) {
                const auto* sourceRow = reinterpret_cast<const float*>(
                    reinterpret_cast<const uint8_t*>(depthData) + static_cast<size_t>(y) * depthRowPitch);
                std::memcpy(depthPixels.data() + static_cast<size_t>(y) * width_, sourceRow,
                            static_cast<size_t>(width_) * sizeof(float));
            });

            header.channels().insert("depth.Z", Imf::Channel(Imf::FLOAT));
            framebuffer.insert("depth.Z", Imf::Slice(Imf::FLOAT,
                reinterpret_cast<char*>(depthPixels.data()), sizeof(float), sizeof(float) * width_));
        }

        std::stringstream filenameStream;
        filenameStream << outputPath_ << "\\frame."
                       << std::setw(5) << std::setfill('0') << frameNumber
                       << ".exr";

        try {
            Imf::OutputFile file(filenameStream.str().c_str(), header);
            file.setFrameBuffer(framebuffer);
            file.writePixels(height_);
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "Enhanced OpenEXR write failed: ", ex.what());
            POST();
            return E_FAIL;
        }

        LOG(LL_NFO, "Exported Enhanced EXR frame: ", frameNumber);
        POST();
        return S_OK;
    }
}
