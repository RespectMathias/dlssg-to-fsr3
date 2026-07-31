#include "vulkan_ngx_e2e_backend.h"
#include "../../support/image_metrics.h"
#include "../../support/procedural_scene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vulkan_ngx_e2e
{
	namespace
	{
		constexpr uint32_t kNgxSuccess = 0x1;
		constexpr uint32_t kNgxInvalidParameter = 0xBAD00005;
		constexpr uint8_t kOutputSentinel = 0x5a;

		FrameInput SolidFrame(uint32_t width, uint32_t height, std::array<uint8_t, 4> color)
		{
			const size_t pixels = static_cast<size_t>(width) * height;
			FrameInput input;
			input.colorRgba8.resize(pixels * 4);
			for (size_t i = 0; i < pixels; ++i)
				std::copy(color.begin(), color.end(), input.colorRgba8.begin() + static_cast<ptrdiff_t>(i * 4));
			input.depth.assign(pixels, 0.5f);
			input.motion.assign(pixels, { 0.0f, 0.0f });
			return input;
		}

		bool MostlyWritten(const std::vector<uint8_t>& output)
		{
			if (output.empty() || output.size() % 4 != 0)
				return false;
			size_t changedChannels = 0;
			for (size_t offset = 0; offset < output.size(); offset += 4)
			{
				for (size_t channel = 0; channel < 3; ++channel)
					changedChannels += output[offset + channel] != kOutputSentinel ? 1U : 0U;
			}
			return changedChannels * 10 >= (output.size() / 4) * 27;
		}

		FrameInput SceneInput(const test_support::SceneFrame& frame, bool reset)
		{
			FrameInput input;
			input.colorRgba8 = frame.color_rgba8;
			input.depth = frame.depth;
			input.motion.resize(frame.depth.size());
			for (size_t pixel = 0; pixel < input.motion.size(); ++pixel)
			{
				input.motion[pixel] = {
					test_support::HalfToFloat(frame.motion_rg16f[pixel * 2]),
					test_support::HalfToFloat(frame.motion_rg16f[pixel * 2 + 1]),
				};
			}
			input.enableInterpolation = true;
			input.reset = reset;
			return input;
		}

		void ExpectNoValidationErrors(const Backend& backend)
		{
			if (!backend.ValidationEnabled())
				return;
			ASSERT_TRUE(backend.ValidationErrors().empty()) << backend.ValidationErrors().front();
		}

		std::unique_ptr<Backend> CreateBackend(const Config& config, std::string* error)
		{
			auto backend = std::make_unique<Backend>();
			if (!backend->Initialize(config))
			{
				*error = backend->LastError();
				if (!backend->Unsupported())
					throw std::runtime_error(*error);
				return {};
			}
			return backend;
		}

		TEST(VulkanNgxInterpolation, LegacySubmitShaderCopyResetWarmup)
		{
			constexpr uint32_t width = 64;
			constexpr uint32_t height = 64;
			constexpr std::array<uint8_t, 4> stableColor = { 44, 101, 173, 255 };
			const Config config = {
				.width = width,
				.height = height,
				.submitApi = SubmitApi::Legacy,
				.copyPath = CopyPath::Shader,
			};
			std::string initializationError;
			auto backend = CreateBackend(config, &initializationError);
			if (!backend)
				GTEST_SKIP() << initializationError;

			auto warmup = SolidFrame(width, height, stableColor);
			warmup.enableInterpolation = true;
			warmup.reset = true;
			FrameOutput warmupOutput;
			ASSERT_TRUE(backend->RunFrame(warmup, &warmupOutput)) << backend->LastError();
			EXPECT_TRUE(warmupOutput.ngxResult == kNgxSuccess || warmupOutput.ngxResult == kNgxInvalidParameter);
			EXPECT_EQ(warmupOutput.outputRealRgba8, warmup.colorRgba8);

			auto steady = SolidFrame(width, height, stableColor);
			steady.enableInterpolation = true;
			FrameOutput steadyOutput;
			ASSERT_TRUE(backend->RunFrame(steady, &steadyOutput)) << backend->LastError();
			EXPECT_EQ(steadyOutput.ngxResult, kNgxSuccess);
			EXPECT_EQ(steadyOutput.outputRealRgba8, steady.colorRgba8);
			ASSERT_EQ(steadyOutput.outputInterpolatedRgba8.size(), steady.colorRgba8.size());

			size_t differingRgbBytes = 0;
			for (size_t i = 0; i < steady.colorRgba8.size(); i += 4)
			{
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const int difference = static_cast<int>(steadyOutput.outputInterpolatedRgba8[i + channel]) -
						steady.colorRgba8[i + channel];
					if (difference != 0)
						++differingRgbBytes;
					EXPECT_LE(std::abs(difference), 2) << "byte " << i + channel;
				}
				EXPECT_EQ(steadyOutput.outputInterpolatedRgba8[i + 3], 0) << "alpha byte " << i + 3;
			}
			EXPECT_LT(differingRgbBytes, steady.colorRgba8.size() / 20);
			EXPECT_FALSE(std::ranges::all_of(steadyOutput.outputInterpolatedRgba8, [](uint8_t value)
			{
				return value == kOutputSentinel;
			}));
			backend->Shutdown();
			ExpectNoValidationErrors(*backend);
		}

		TEST(VulkanNgxPassthrough, Submit2TransferCopyExactOutputReal)
		{
			constexpr uint32_t width = 64;
			constexpr uint32_t height = 64;
			const Config config = {
				.width = width,
				.height = height,
				.submitApi = SubmitApi::Submit2,
				.copyPath = CopyPath::Transfer,
			};
			std::string initializationError;
			auto backend = CreateBackend(config, &initializationError);
			if (!backend)
				GTEST_SKIP() << initializationError;
			auto input = SolidFrame(width, height, { 17, 93, 201, 255 });
			input.enableInterpolation = false;
			input.reset = true;
			FrameOutput output;
			ASSERT_TRUE(backend->RunFrame(input, &output)) << backend->LastError();
			EXPECT_EQ(output.ngxResult, kNgxSuccess);
			EXPECT_EQ(output.outputRealRgba8, input.colorRgba8);
			EXPECT_TRUE(std::ranges::all_of(output.outputInterpolatedRgba8, [](uint8_t value)
			{
				return value == kOutputSentinel;
			}));
			backend->Shutdown();
			ExpectNoValidationErrors(*backend);
		}

		TEST(VulkanNgxInterpolation, ProceduralMidpointBeatsBothEndpoints)
		{
			const auto scene = test_support::GenerateProceduralScene();
			const Config config = {
				.width = test_support::scene_width,
				.height = test_support::scene_height,
				.submitApi = SubmitApi::Legacy,
				.copyPath = CopyPath::Shader,
			};
			std::string initializationError;
			auto backend = CreateBackend(config, &initializationError);
			if (!backend)
				GTEST_SKIP() << initializationError;

			FrameOutput firstOutput;
			ASSERT_TRUE(backend->RunFrame(SceneInput(scene.first, true), &firstOutput)) << backend->LastError();
			FrameOutput secondOutput;
			ASSERT_TRUE(backend->RunFrame(SceneInput(scene.second, false), &secondOutput)) << backend->LastError();
			ASSERT_EQ(secondOutput.ngxResult, kNgxSuccess);

			const auto generated = test_support::ComputeImageMetrics(
				secondOutput.outputInterpolatedRgba8,
				scene.midpoint_reference.color_rgba8,
				test_support::scene_width,
				test_support::scene_height);
			const auto firstEndpoint = test_support::ComputeImageMetrics(
				scene.first.color_rgba8,
				scene.midpoint_reference.color_rgba8,
				test_support::scene_width,
				test_support::scene_height);
			const auto secondEndpoint = test_support::ComputeImageMetrics(
				scene.second.color_rgba8,
				scene.midpoint_reference.color_rgba8,
				test_support::scene_width,
				test_support::scene_height);

			EXPECT_LT(generated.mae, std::min(firstEndpoint.mae, secondEndpoint.mae));
			EXPECT_GT(generated.psnr, std::max(firstEndpoint.psnr, secondEndpoint.psnr));
			backend->Shutdown();
			ExpectNoValidationErrors(*backend);
		}

		TEST(VulkanNgxStress, Submit2TransferToggleWithoutRecreatingFeature)
		{
			constexpr uint32_t width = 64;
			constexpr uint32_t height = 64;
			const Config config = {
				.width = width,
				.height = height,
				.submitApi = SubmitApi::Submit2,
				.copyPath = CopyPath::Transfer,
			};
			std::string initializationError;
			auto backend = CreateBackend(config, &initializationError);
			if (!backend)
				GTEST_SKIP() << initializationError;

			bool previousEnabled = false;
			for (uint32_t frameIndex = 0; frameIndex < 24; ++frameIndex)
			{
				const bool enabled = (frameIndex / 4) % 2 != 0;
				const bool reset = enabled && !previousEnabled;
				auto input = SolidFrame(
					width,
					height,
					{ static_cast<uint8_t>(20 + frameIndex * 3), static_cast<uint8_t>(80 + frameIndex), 170, 255 });
				input.enableInterpolation = enabled;
				input.reset = reset;
				FrameOutput output;
				ASSERT_TRUE(backend->RunFrame(input, &output)) << backend->LastError();
				EXPECT_EQ(output.outputRealRgba8, input.colorRgba8) << frameIndex;
				if (enabled && !reset)
				{
					EXPECT_TRUE(MostlyWritten(output.outputInterpolatedRgba8)) << frameIndex;
				}
				previousEnabled = enabled;
			}
			backend->Shutdown();
			ExpectNoValidationErrors(*backend);
		}
	}
}
