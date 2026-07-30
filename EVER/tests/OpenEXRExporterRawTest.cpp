#include "OpenEXRExporter.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <Imath/half.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
    constexpr int kWidth = 2;
    constexpr int kHeight = 2;
    constexpr uint32_t kColorRowPitch = 12;
    constexpr uint32_t kDepthRowPitch = 12;

    bool closeEnough(float actual, float expected, float tolerance = 0.002f) {
        return std::fabs(actual - expected) <= tolerance;
    }

    int fail(const std::string& message) {
        std::cerr << "OpenEXR raw export test failed: " << message << '\n';
        return 1;
    }

    template <typename T>
    void printStats(const char* name, const std::vector<T>& values) {
        double sum = 0.0;
        float minimum = std::numeric_limits<float>::infinity();
        float maximum = -std::numeric_limits<float>::infinity();
        size_t finiteCount = 0;
        for (const T& rawValue : values) {
            const float value = static_cast<float>(rawValue);
            if (!std::isfinite(value)) continue;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            sum += value;
            ++finiteCount;
        }
        std::cout << name << ": finite=" << finiteCount << '/' << values.size()
                  << " min=" << minimum << " max=" << maximum
                  << " mean=" << (finiteCount == 0 ? 0.0 : sum / finiteCount)
                  << " varying=" << (finiteCount != 0 && maximum != minimum) << '\n';
    }

    int inspectFile(const std::filesystem::path& inputPath) {
        try {
            Imf::InputFile input(inputPath.string().c_str());
            const Imf::Header& header = input.header();
            const Imath::Box2i dataWindow = header.dataWindow();
            const int width = dataWindow.max.x - dataWindow.min.x + 1;
            const int height = dataWindow.max.y - dataWindow.min.y + 1;
            if (width <= 0 || height <= 0 || dataWindow.min.x != 0 || dataWindow.min.y != 0) {
                return fail("inspector requires a positive zero-origin data window");
            }

            std::cout << "file=" << inputPath << '\n';
            std::cout << "dimensions=" << width << 'x' << height << '\n';
            for (const char* channel : {"R", "G", "B", "SubsurfaceScatter", "depth.Z"}) {
                std::cout << "channel." << channel << '='
                          << (header.channels().findChannel(channel) != nullptr) << '\n';
            }

            const size_t pixelCount = static_cast<size_t>(width) * height;
            std::vector<Imath::half> red(pixelCount);
            std::vector<Imath::half> green(pixelCount);
            std::vector<Imath::half> blue(pixelCount);
            std::vector<Imath::half> scatter(pixelCount);
            std::vector<float> depth(pixelCount);
            Imf::FrameBuffer frameBuffer;
            const size_t halfRowStride = sizeof(Imath::half) * static_cast<size_t>(width);
            const size_t floatRowStride = sizeof(float) * static_cast<size_t>(width);
            frameBuffer.insert("R", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(red.data()),
                sizeof(Imath::half), halfRowStride));
            frameBuffer.insert("G", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(green.data()),
                sizeof(Imath::half), halfRowStride));
            frameBuffer.insert("B", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(blue.data()),
                sizeof(Imath::half), halfRowStride));
            frameBuffer.insert("SubsurfaceScatter", Imf::Slice(Imf::HALF,
                reinterpret_cast<char*>(scatter.data()), sizeof(Imath::half), halfRowStride));
            frameBuffer.insert("depth.Z", Imf::Slice(Imf::FLOAT,
                reinterpret_cast<char*>(depth.data()), sizeof(float), floatRowStride));
            input.setFrameBuffer(frameBuffer);
            input.readPixels(dataWindow.min.y, dataWindow.max.y);

            printStats("R", red);
            printStats("G", green);
            printStats("B", blue);
            printStats("SubsurfaceScatter", scatter);
            printStats("depth.Z", depth);
            return 0;
        } catch (const std::exception& ex) {
            return fail(ex.what());
        }
    }
}

int main(int argc, char** argv) {
    if (argc == 2) return inspectFile(argv[1]);
    const std::filesystem::path outputDirectory =
        std::filesystem::current_path() / "openexr_raw_test_output";
    const std::filesystem::path outputFile = outputDirectory / "frame.00000.exr";

    // Both source planes deliberately contain end-of-row padding, matching the
    // aligned row pitches returned by D3D12 readback resources.
    const std::array<uint8_t, kColorRowPitch * kHeight> rgba = {
        255,   0, 128,  64,
          0, 255,  32, 255,
        222, 222, 222, 222,
         10,  20,  30,  40,
        200, 150, 100,  50,
        111, 111, 111, 111,
    };
    const std::array<float, (kDepthRowPitch / sizeof(float)) * kHeight> depth = {
        0.0f, 0.25f, -99.0f,
        0.5f, 1.0f, -99.0f,
    };
    const std::array<float, kWidth * kHeight> expectedDepth = {0.0f, 0.25f, 0.5f, 1.0f};

    Encoder::OpenEXRExporter exporter;
    exporter.initialize(outputDirectory.string(), kWidth, kHeight);
    const HRESULT exportResult = exporter.exportFrame(
        rgba.data(), kColorRowPitch, depth.data(), kDepthRowPitch, 0);
    if (FAILED(exportResult)) return fail("exportFrame returned a failure HRESULT");
    if (!std::filesystem::is_regular_file(outputFile)) return fail("output frame was not created");

    try {
        Imf::InputFile input(outputFile.string().c_str());
        const Imf::Header& header = input.header();
        const Imath::Box2i dataWindow = header.dataWindow();
        if (dataWindow.min.x != 0 || dataWindow.min.y != 0 ||
            dataWindow.max.x != kWidth - 1 || dataWindow.max.y != kHeight - 1) {
            return fail("data window dimensions are incorrect");
        }

        for (const char* channel : {"R", "G", "B", "SubsurfaceScatter", "depth.Z"}) {
            if (header.channels().findChannel(channel) == nullptr) {
                return fail(std::string("missing channel ") + channel);
            }
        }

        std::array<Imath::half, kWidth * kHeight> red{};
        std::array<Imath::half, kWidth * kHeight> green{};
        std::array<Imath::half, kWidth * kHeight> blue{};
        std::array<Imath::half, kWidth * kHeight> scatter{};
        std::array<float, kWidth * kHeight> decodedDepth{};
        Imf::FrameBuffer frameBuffer;
        frameBuffer.insert("R", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(red.data()),
            sizeof(Imath::half), sizeof(Imath::half) * kWidth));
        frameBuffer.insert("G", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(green.data()),
            sizeof(Imath::half), sizeof(Imath::half) * kWidth));
        frameBuffer.insert("B", Imf::Slice(Imf::HALF, reinterpret_cast<char*>(blue.data()),
            sizeof(Imath::half), sizeof(Imath::half) * kWidth));
        frameBuffer.insert("SubsurfaceScatter", Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(scatter.data()), sizeof(Imath::half),
            sizeof(Imath::half) * kWidth));
        frameBuffer.insert("depth.Z", Imf::Slice(Imf::FLOAT,
            reinterpret_cast<char*>(decodedDepth.data()), sizeof(float), sizeof(float) * kWidth));
        input.setFrameBuffer(frameBuffer);
        input.readPixels(0, kHeight - 1);

        if (!closeEnough(static_cast<float>(red[0]), 1.0f) ||
            !closeEnough(static_cast<float>(green[0]), 0.0f) ||
            !closeEnough(static_cast<float>(blue[0]), 128.0f / 255.0f) ||
            !closeEnough(static_cast<float>(scatter[0]), 64.0f / 255.0f)) {
            return fail("RGBA channel values do not match the source frame");
        }
        if (!closeEnough(static_cast<float>(red[3]), 200.0f / 255.0f) ||
            !closeEnough(static_cast<float>(green[3]), 150.0f / 255.0f) ||
            !closeEnough(static_cast<float>(blue[3]), 100.0f / 255.0f) ||
            !closeEnough(static_cast<float>(scatter[3]), 50.0f / 255.0f)) {
            return fail("padded color rows were decoded incorrectly");
        }
        for (size_t i = 0; i < expectedDepth.size(); ++i) {
            if (!closeEnough(decodedDepth[i], expectedDepth[i], 0.00001f)) {
                return fail("depth.Z values do not match the source plane");
            }
        }
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }

    std::cout << "OpenEXR raw export test passed: " << outputFile << '\n';
    return 0;
}
