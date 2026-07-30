#include "dx12/dx12_ngx_backend.h"
#include "../support/image_metrics.h"
#include "../support/procedural_scene.h"
#include "vulkan/vulkan_ngx_e2e_backend.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <cstddef>
#include <iterator>
#include <string>

namespace
{
	std::wstring DllPath()
	{
		wchar_t path[32768] = {};
		const auto length = GetEnvironmentVariableW(L"DLSSG_TO_FSR_DLL", path, static_cast<DWORD>(std::size(path)));
		return length > 0 && length < std::size(path) ? std::wstring(path, length) : std::wstring {};
	}

	vulkan_ngx_e2e::FrameInput VulkanInput(const test_support::SceneFrame& frame, bool reset)
	{
		vulkan_ngx_e2e::FrameInput input;
		input.colorRgba8 = frame.color_rgba8;
		input.depth = frame.depth;
		input.motion.resize(frame.depth.size());
		for (std::size_t pixel = 0; pixel < input.motion.size(); ++pixel)
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

	TEST(BackendParity, VulkanMatchesDx12ForProceduralSequence)
	{
		const auto dll = DllPath();
		if (dll.empty())
			GTEST_SKIP() << "DLSSG_TO_FSR_DLL is unset";
		const auto scene = test_support::GenerateProceduralScene();

		dx12_ngx_e2e::BackendConfig dxConfig;
		dxConfig.dll = dll;
		dxConfig.width = test_support::scene_width;
		dxConfig.height = test_support::scene_height;
		auto dxBackend = dx12_ngx_e2e::Dx12NgxBackend::Create(dxConfig);
		const auto dxInput = [](const test_support::SceneFrame& frame)
		{
			return dx12_ngx_e2e::FrameInput { frame.color_rgba8, frame.depth, frame.motion_rg16f };
		};
		static_cast<void>(dxBackend->RunFrame(dxInput(scene.first), true, true));
		auto dxOutput = dxBackend->RunFrame(dxInput(scene.second), true, false).outputInterpolated;
		dxBackend.reset();

		vulkan_ngx_e2e::Config vkConfig;
		vkConfig.width = test_support::scene_width;
		vkConfig.height = test_support::scene_height;
		vkConfig.dll = dll;
		vkConfig.submitApi = vulkan_ngx_e2e::SubmitApi::Legacy;
		vkConfig.copyPath = vulkan_ngx_e2e::CopyPath::Shader;
		vulkan_ngx_e2e::Backend vkBackend;
		if (!vkBackend.Initialize(vkConfig))
		{
			if (vkBackend.Unsupported())
				GTEST_SKIP() << vkBackend.LastError();
			FAIL() << vkBackend.LastError();
		}
		vulkan_ngx_e2e::FrameOutput firstOutput;
		ASSERT_TRUE(vkBackend.RunFrame(VulkanInput(scene.first, true), &firstOutput)) << vkBackend.LastError();
		vulkan_ngx_e2e::FrameOutput secondOutput;
		ASSERT_TRUE(vkBackend.RunFrame(VulkanInput(scene.second, false), &secondOutput)) << vkBackend.LastError();

		const auto parity = test_support::ComputeImageMetrics(
			secondOutput.outputInterpolatedRgba8,
			dxOutput,
			test_support::scene_width,
			test_support::scene_height);
		EXPECT_LT(parity.mae, 1.0);
		EXPECT_GT(parity.psnr, 45.0);
		vkBackend.Shutdown();
		EXPECT_TRUE(vkBackend.ValidationErrors().empty());
	}
}
