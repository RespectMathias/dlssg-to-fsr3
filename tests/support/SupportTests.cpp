#include <gtest/gtest.h>

#include "test_support.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t sequence = 0U;
        const auto base = std::filesystem::temp_directory_path();
        for (std::size_t attempt = 0U; attempt < 100U; ++attempt)
        {
            const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            auto candidate = base / ("dlssg_support_tests_" + std::to_string(timestamp) + "_" +
                                     std::to_string(sequence.fetch_add(1U)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error))
            {
                path_ = std::move(candidate);
                return;
            }
            if (error)
                throw std::filesystem::filesystem_error("Could not create test directory", candidate, error);
        }
        throw std::runtime_error("Could not allocate a unique test directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(path_, error));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> ReadPrefix(const std::filesystem::path& path, std::size_t count)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    std::vector<std::uint8_t> bytes(count);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return bytes;
}

TEST(NgxParameterBag, DispatchesSettersAndGettersThroughAbi)
{
    test_support::NgxParameterBag bag;
    test_support::NgxInstanceParameters& parameters = bag;
    int first_pointer = 1;
    int second_pointer = 2;
    int resource_storage = 3;
    auto *resource = reinterpret_cast<ID3D12Resource *>(&resource_storage);

    parameters.SetVoidPointer("void", &first_pointer);
    parameters.Set2("float", 1.25F);
    parameters.Set3("set3", &second_pointer);
    parameters.Set4("set4", 17U);
    parameters.Set5("set5", 29U);
    parameters.Set6("set6", &first_pointer);
    parameters.Set7("set7", resource);
    parameters.Set8("set8", &second_pointer);

    EXPECT_EQ(bag.Size(), 8U);
    EXPECT_TRUE(bag.Contains("set7"));

    void *pointer = nullptr;
    EXPECT_EQ(parameters.GetVoidPointer("void", &pointer), test_support::ngx_success);
    EXPECT_EQ(pointer, &first_pointer);

    float float_value = 0.0F;
    EXPECT_EQ(parameters.Get2("float", &float_value), test_support::ngx_success);
    EXPECT_FLOAT_EQ(float_value, 1.25F);
    float_value = 0.0F;
    EXPECT_EQ(parameters.Get7("float", &float_value), test_support::ngx_success);
    EXPECT_FLOAT_EQ(float_value, 1.25F);

    pointer = nullptr;
    EXPECT_EQ(parameters.Get3("set3", &pointer), test_support::ngx_success);
    EXPECT_EQ(pointer, &second_pointer);

    std::uint32_t uint_value = 0U;
    EXPECT_EQ(parameters.Get4("set4", &uint_value), test_support::ngx_success);
    EXPECT_EQ(uint_value, 17U);
    uint_value = 0U;
    EXPECT_EQ(parameters.Get5("set5", &uint_value), test_support::ngx_success);
    EXPECT_EQ(uint_value, 29U);

    pointer = nullptr;
    EXPECT_EQ(parameters.Get6("set6", &pointer), test_support::ngx_success);
    EXPECT_EQ(pointer, &first_pointer);
    pointer = nullptr;
    EXPECT_EQ(parameters.GetVoidPointer("set7", &pointer), test_support::ngx_success);
    EXPECT_EQ(pointer, static_cast<void *>(resource));
    pointer = nullptr;
    EXPECT_EQ(parameters.Get8("set8", &pointer), test_support::ngx_success);
    EXPECT_EQ(pointer, &second_pointer);

    parameters.Unknown();
    EXPECT_EQ(bag.Size(), 0U);
}

TEST(NgxParameterBag, MissesReturnInvalidParameterWithoutChangingOutputs)
{
    test_support::NgxParameterBag bag;
    test_support::NgxInstanceParameters& parameters = bag;
    int sentinel = 0;
    void *pointer = &sentinel;
    float float_value = 4.5F;
    std::uint32_t uint_value = 41U;

    EXPECT_EQ(parameters.GetVoidPointer("missing", &pointer), test_support::ngx_invalid_parameter);
    EXPECT_EQ(pointer, &sentinel);
    EXPECT_EQ(parameters.Get2("missing", &float_value), test_support::ngx_invalid_parameter);
    EXPECT_FLOAT_EQ(float_value, 4.5F);
    EXPECT_EQ(parameters.Get3("missing", &pointer), test_support::ngx_invalid_parameter);
    EXPECT_EQ(pointer, &sentinel);
    EXPECT_EQ(parameters.Get4("missing", &uint_value), test_support::ngx_invalid_parameter);
    EXPECT_EQ(uint_value, 41U);
    EXPECT_EQ(parameters.Get5("missing", &uint_value), test_support::ngx_invalid_parameter);
    EXPECT_EQ(uint_value, 41U);
    EXPECT_EQ(parameters.Get6("missing", &pointer), test_support::ngx_invalid_parameter);
    EXPECT_EQ(pointer, &sentinel);
    EXPECT_EQ(parameters.Get7("missing", &float_value), test_support::ngx_invalid_parameter);
    EXPECT_FLOAT_EQ(float_value, 4.5F);
    EXPECT_EQ(parameters.Get8("missing", &pointer), test_support::ngx_invalid_parameter);
    EXPECT_EQ(pointer, &sentinel);
}

TEST(ProceduralScene, HasExpectedFrameSizesAndMidpointMotion)
{
    const test_support::ProceduralScene scene = test_support::GenerateProceduralScene();
    constexpr std::size_t pixel_count =
        static_cast<std::size_t>(test_support::scene_width) * test_support::scene_height;
    const auto expect_frame_size = [pixel_count](const test_support::SceneFrame& frame)
    {
        EXPECT_EQ(frame.width, test_support::scene_width);
        EXPECT_EQ(frame.height, test_support::scene_height);
        EXPECT_EQ(frame.color_rgba8.size(), pixel_count * 4U);
        EXPECT_EQ(frame.depth.size(), pixel_count);
        EXPECT_EQ(frame.motion_rg16f.size(), pixel_count * 2U);
    };

    expect_frame_size(scene.first);
    expect_frame_size(scene.second);
    expect_frame_size(scene.midpoint_reference);

    const auto expect_motion = [&scene](std::uint32_t x, std::uint32_t y, float expected_x, float expected_y)
    {
        const std::size_t pixel = static_cast<std::size_t>(y) * test_support::scene_width + x;
        EXPECT_FLOAT_EQ(test_support::HalfToFloat(scene.midpoint_reference.motion_rg16f[pixel * 2U]), expected_x);
        EXPECT_FLOAT_EQ(test_support::HalfToFloat(scene.midpoint_reference.motion_rg16f[pixel * 2U + 1U]), expected_y);
    };

    expect_motion(
        197U,
        142U,
        test_support::HalfToFloat(test_support::FloatToHalf(-96.0F / test_support::scene_width)),
        test_support::HalfToFloat(test_support::FloatToHalf(-34.0F / test_support::scene_height)));
    expect_motion(
        437U,
        211U,
        test_support::HalfToFloat(test_support::FloatToHalf(72.0F / test_support::scene_width)),
        test_support::HalfToFloat(test_support::FloatToHalf(48.0F / test_support::scene_height)));
    expect_motion(0U, 0U, 0.0F, 0.0F);
}

TEST(HalfConversion, FloatToHalfHandlesEdgeValues)
{
    EXPECT_EQ(test_support::FloatToHalf(0.0F), 0x0000U);
    EXPECT_EQ(test_support::FloatToHalf(-0.0F), 0x8000U);
    EXPECT_EQ(test_support::FloatToHalf(1.0F), 0x3C00U);
    EXPECT_EQ(test_support::FloatToHalf(-2.0F), 0xC000U);
    EXPECT_EQ(test_support::FloatToHalf(0x1p-24F), 0x0001U);
    EXPECT_EQ(test_support::FloatToHalf(0x1p-14F), 0x0400U);
    EXPECT_EQ(test_support::FloatToHalf(65504.0F), 0x7BFFU);
    EXPECT_EQ(test_support::FloatToHalf(std::numeric_limits<float>::infinity()), 0x7C00U);
    EXPECT_EQ(test_support::FloatToHalf(-std::numeric_limits<float>::infinity()), 0xFC00U);
    EXPECT_EQ(test_support::FloatToHalf(std::numeric_limits<float>::quiet_NaN()) & 0x7FFFU, 0x7E00U);
}

TEST(HalfConversion, HalfToFloatHandlesEdgeValues)
{
    EXPECT_FLOAT_EQ(test_support::HalfToFloat(0x0000U), 0.0F);
    EXPECT_TRUE(std::signbit(test_support::HalfToFloat(0x8000U)));
    EXPECT_FLOAT_EQ(test_support::HalfToFloat(0x3C00U), 1.0F);
    EXPECT_FLOAT_EQ(test_support::HalfToFloat(0xC000U), -2.0F);
    EXPECT_FLOAT_EQ(test_support::HalfToFloat(0x0001U), 0x1p-24F);
    EXPECT_FLOAT_EQ(test_support::HalfToFloat(0x0400U), 0x1p-14F);
    EXPECT_FLOAT_EQ(test_support::HalfToFloat(0x7BFFU), 65504.0F);
    EXPECT_EQ(test_support::HalfToFloat(0x7C00U), std::numeric_limits<float>::infinity());
    EXPECT_EQ(test_support::HalfToFloat(0xFC00U), -std::numeric_limits<float>::infinity());
    EXPECT_TRUE(std::isnan(test_support::HalfToFloat(0x7E00U)));
}

TEST(ImageMetrics, ExactImagesHaveZeroError)
{
    constexpr std::array<std::uint8_t, 16U> image = {
        1U, 2U, 3U, 4U,
        5U, 6U, 7U, 8U,
        9U, 10U, 11U, 12U,
        13U, 14U, 15U, 16U,
    };

    const auto metrics = test_support::ComputeImageMetrics(image, image, 2U, 2U);
    EXPECT_DOUBLE_EQ(metrics.mae, 0.0);
    EXPECT_TRUE(std::isinf(metrics.psnr));
    EXPECT_DOUBLE_EQ(metrics.error_centroid_x, 0.5);
    EXPECT_DOUBLE_EQ(metrics.error_centroid_y, 0.5);
}

TEST(ImageMetrics, DifferentImagesReportExpectedError)
{
    constexpr std::array<std::uint8_t, 16U> reference = {};
    constexpr std::array<std::uint8_t, 16U> actual = {
        0U, 0U, 0U, 0U,
        255U, 255U, 255U, 0U,
        0U, 0U, 0U, 0U,
        0U, 0U, 0U, 255U,
    };

    const auto metrics = test_support::ComputeImageMetrics(actual, reference, 2U, 2U);
    EXPECT_DOUBLE_EQ(metrics.mae, 63.75);
    EXPECT_NEAR(metrics.psnr, 6.020599913279624, 1.0e-12);
    EXPECT_DOUBLE_EQ(metrics.error_centroid_x, 1.0);
    EXPECT_DOUBLE_EQ(metrics.error_centroid_y, 0.0);
}

TEST(RollingFrameStatistics, ComputesRollingPercentilesAndFps)
{
    test_support::RollingFrameStatistics statistics(4U);
    statistics.AddMilliseconds(10.0);
    statistics.AddMilliseconds(20.0);
    statistics.AddMilliseconds(30.0);
    statistics.AddMilliseconds(40.0);
    statistics.Add(std::chrono::duration<double, std::milli>(50.0));

    const auto snapshot = statistics.Snapshot();
    EXPECT_EQ(statistics.Capacity(), 4U);
    EXPECT_EQ(statistics.Size(), 4U);
    EXPECT_EQ(snapshot.sample_count, 4U);
    EXPECT_DOUBLE_EQ(snapshot.average_ms, 35.0);
    EXPECT_NEAR(snapshot.fps, 1000.0 / 35.0, 1.0e-12);
    EXPECT_DOUBLE_EQ(snapshot.minimum_ms, 20.0);
    EXPECT_DOUBLE_EQ(snapshot.maximum_ms, 50.0);
    EXPECT_DOUBLE_EQ(snapshot.p50_ms, 35.0);
    EXPECT_DOUBLE_EQ(snapshot.p95_ms, 48.5);
    EXPECT_DOUBLE_EQ(snapshot.p99_ms, 49.7);
}

TEST(JsonReport, EscapesJsonSpecialCharacters)
{
    TemporaryDirectory directory;
    const auto path = directory.Path() / "report.json";
    test_support::JsonReport report;
    report.name = "quote\" slash\\ back\b form\f line\n return\r tab\t control";
    report.name.push_back('\x01');
    test_support::WriteJsonReport(path, report);

    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string contents { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    const std::string expected =
        "{\n"
        "  \"name\": \"quote\\\" slash\\\\ back\\b form\\f line\\n return\\r tab\\t control\\u0001\",\n"
        "  \"passed\": false,\n"
        "  \"metadata\": {},\n"
        "  \"metrics\": {},\n"
        "  \"artifacts\": []\n"
        "}\n";
    EXPECT_EQ(contents, expected);
}

TEST(WicImageWriter, WritesPngAndBmpToTemporaryDirectory)
{
    TemporaryDirectory directory;
    const auto png_path = directory.Path() / "artifact.png";
    const auto bmp_path = directory.Path() / "artifact.bmp";
    constexpr std::array<std::uint8_t, 16U> rgba = {
        255U, 0U, 0U, 255U,
        0U, 255U, 0U, 255U,
        0U, 0U, 255U, 255U,
        255U, 255U, 255U, 128U,
    };

    test_support::WriteImageArtifact(png_path, 2U, 2U, rgba);
    test_support::WriteImageArtifact(bmp_path, 2U, 2U, rgba);

    constexpr std::array<std::uint8_t, 8U> png_signature = { 0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU };
    constexpr std::array<std::uint8_t, 2U> bmp_signature = { 0x42U, 0x4DU };
    EXPECT_EQ(ReadPrefix(png_path, png_signature.size()),
              std::vector<std::uint8_t>(png_signature.begin(), png_signature.end()));
    EXPECT_EQ(ReadPrefix(bmp_path, bmp_signature.size()),
              std::vector<std::uint8_t>(bmp_signature.begin(), bmp_signature.end()));
    EXPECT_GT(std::filesystem::file_size(png_path), png_signature.size());
    EXPECT_GT(std::filesystem::file_size(bmp_path), bmp_signature.size());
}
}
