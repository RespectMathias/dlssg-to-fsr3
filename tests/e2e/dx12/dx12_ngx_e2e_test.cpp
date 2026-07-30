#include "dx12_ngx_backend.h"
#include "../../support/image_metrics.h"
#include "../../support/procedural_scene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace dx12_ngx_e2e
{
	namespace
	{
		class Dx12NgxE2eTest : public testing::TestWithParam<RecordingPath>
		{
		protected:
			void SetUp() override
			{
				const BackendConfig config = BackendConfig::FromEnvironment();
				if (config.dll.empty())
					GTEST_SKIP() << "Set DLSSG_TO_FSR3_DLL to DLL path";
				try
				{
					backend_ = Dx12NgxBackend::Create(config);
				}
				catch (const std::exception& error)
				{
					if (std::string_view(error.what()).starts_with("No hardware D3D12"))
						GTEST_SKIP() << error.what();
					FAIL() << error.what();
				}
			}

			std::unique_ptr<Dx12NgxBackend> backend_;
		};

		TEST_P(Dx12NgxE2eTest, DisabledInterpolationCopiesBackbufferExactly)
		{
			const FrameResult frame = backend_->RunFrame(7, false, true, GetParam());

			ASSERT_EQ(frame.submittedColor.size(), frame.outputReal.size());
			EXPECT_TRUE(std::equal(frame.submittedColor.begin(), frame.submittedColor.end(), frame.outputReal.begin()));
			EXPECT_GT(frame.timings.total.count(), 0);
		}

		TEST_P(Dx12NgxE2eTest, InterpolationProducesValidSpatialOutputAfterResetWarmup)
		{
			static_cast<void>(backend_->WarmupInterpolation(0, GetParam()));
			const FrameResult frame = backend_->RunFrame(1, true, false, GetParam());

			ASSERT_EQ(frame.submittedColor.size(), frame.outputReal.size());
			ASSERT_EQ(frame.interpolationBaseline.size(), frame.outputInterpolated.size());
			EXPECT_TRUE(std::equal(frame.submittedColor.begin(), frame.submittedColor.end(), frame.outputReal.begin()));

			std::size_t changedPixels = 0;
			std::unordered_set<std::uint32_t> colors;
			for (std::size_t offset = 0; offset < frame.outputInterpolated.size(); offset += 4U)
			{
				const bool changed = !std::equal(
					frame.outputInterpolated.begin() + static_cast<std::ptrdiff_t>(offset),
					frame.outputInterpolated.begin() + static_cast<std::ptrdiff_t>(offset + 4U),
					frame.interpolationBaseline.begin() + static_cast<std::ptrdiff_t>(offset));
				changedPixels += changed ? 1U : 0U;
				const std::uint32_t color = static_cast<std::uint32_t>(frame.outputInterpolated[offset]) |
											(static_cast<std::uint32_t>(frame.outputInterpolated[offset + 1U]) << 8U) |
											(static_cast<std::uint32_t>(frame.outputInterpolated[offset + 2U]) << 16U);
				colors.insert(color);
			}

			const std::size_t pixelCount = static_cast<std::size_t>(frame.width) * frame.height;
			EXPECT_GT(changedPixels, pixelCount / 2U);
			EXPECT_GT(colors.size(), 16U);
			EXPECT_GE(backend_->TimingHistory().size(), 3U);
		}

		INSTANTIATE_TEST_SUITE_P(
			RecordingOwnership,
			Dx12NgxE2eTest,
			testing::Values(RecordingPath::callerOwned, RecordingPath::dllManaged),
			[](const testing::TestParamInfo<RecordingPath>& info)
		{
			return info.param == RecordingPath::callerOwned ? "CallerOwned" : "DllManaged";
		});

		TEST(Dx12NgxQuality, ProceduralMidpointBeatsBothEndpoints)
		{
			BackendConfig config = BackendConfig::FromEnvironment();
			if (config.dll.empty())
				GTEST_SKIP() << "Set DLSSG_TO_FSR3_DLL to DLL path";
			config.width = test_support::scene_width;
			config.height = test_support::scene_height;
			auto backend = Dx12NgxBackend::Create(config);
			const auto scene = test_support::GenerateProceduralScene();
			const auto toInput = [](const test_support::SceneFrame& frame)
			{
				return FrameInput { frame.color_rgba8, frame.depth, frame.motion_rg16f };
			};

			static_cast<void>(backend->RunFrame(toInput(scene.first), true, true));
			const auto output = backend->RunFrame(toInput(scene.second), true, false);
			const auto generated = test_support::ComputeImageMetrics(
				output.outputInterpolated,
				scene.midpoint_reference.color_rgba8,
				config.width,
				config.height);
			const auto firstEndpoint = test_support::ComputeImageMetrics(
				scene.first.color_rgba8,
				scene.midpoint_reference.color_rgba8,
				config.width,
				config.height);
			const auto secondEndpoint = test_support::ComputeImageMetrics(
				scene.second.color_rgba8,
				scene.midpoint_reference.color_rgba8,
				config.width,
				config.height);

			EXPECT_LT(generated.mae, std::min(firstEndpoint.mae, secondEndpoint.mae));
			EXPECT_GT(generated.psnr, std::max(firstEndpoint.psnr, secondEndpoint.psnr));
		}

		TEST(Dx12NgxStress, ToggleInterpolationWithoutRecreatingFeature)
		{
			BackendConfig config = BackendConfig::FromEnvironment();
			if (config.dll.empty())
				GTEST_SKIP() << "Set DLSSG_TO_FSR3_DLL to DLL path";
			auto backend = Dx12NgxBackend::Create(config);
			bool previousEnabled = false;
			for (std::uint32_t frameIndex = 0; frameIndex < 24; ++frameIndex)
			{
				const bool enabled = (frameIndex / 4) % 2 != 0;
				const bool reset = enabled && !previousEnabled;
				const auto frame = backend->RunFrame(frameIndex, enabled, reset);
				EXPECT_EQ(frame.outputReal, frame.submittedColor) << frameIndex;
				if (enabled && !reset)
					EXPECT_NE(frame.outputInterpolated, frame.interpolationBaseline) << frameIndex;
				previousEnabled = enabled;
			}
		}

		TEST(Dx12NgxCompatibility, StockApiDoesNotUseNgxGpuAllocationCallbacks)
		{
			BackendConfig config = BackendConfig::FromEnvironment();
			if (config.dll.empty())
				GTEST_SKIP() << "Set DLSSG_TO_FSR3_DLL to DLL path";
			config.provideRejectingResourceCallbacks = true;
			auto backend = Dx12NgxBackend::Create(config);
			static_cast<void>(backend->WarmupInterpolation());
			const auto frame = backend->RunFrame(1, true, false);
			EXPECT_NE(frame.outputInterpolated, frame.interpolationBaseline);
		}

		TEST(Dx12NgxCompatibility, DilatedMotionVectorsUseStockPrepareFallback)
		{
			BackendConfig config = BackendConfig::FromEnvironment();
			if (config.dll.empty())
				GTEST_SKIP() << "Set DLSSG_TO_FSR3_DLL to DLL path";
			config.motionVectorsDilated = true;
			auto backend = Dx12NgxBackend::Create(config);
			static_cast<void>(backend->WarmupInterpolation());
			const auto frame = backend->RunFrame(1, true, false);
			EXPECT_NE(frame.outputInterpolated, frame.interpolationBaseline);
		}

		TEST(Dx12NgxCompatibility, IncompatibleHudlessFormatFallsBackToBackbuffer)
		{
			BackendConfig config = BackendConfig::FromEnvironment();
			if (config.dll.empty())
				GTEST_SKIP() << "Set DLSSG_TO_FSR3_DLL to DLL path";
			config.provideIncompatibleHudless = true;
			auto backend = Dx12NgxBackend::Create(config);
			static_cast<void>(backend->WarmupInterpolation());
			const auto frame = backend->RunFrame(1, true, false);
			EXPECT_EQ(frame.outputReal, frame.submittedColor);
			EXPECT_NE(frame.outputInterpolated, frame.interpolationBaseline);
		}
	}
}
