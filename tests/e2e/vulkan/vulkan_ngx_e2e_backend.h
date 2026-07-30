#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vulkan_ngx_e2e
{
	enum class SubmitApi
	{
		Legacy,
		Submit2,
	};

	enum class CopyPath
	{
		Shader,
		Transfer,
	};

	struct Config
	{
		uint32_t width = 64;
		uint32_t height = 64;
		SubmitApi submitApi = SubmitApi::Legacy;
		CopyPath copyPath = CopyPath::Shader;
		bool enableValidation = true;
		std::wstring dll;
		uint64_t waitTimeoutNanoseconds = 30'000'000'000ull;
	};

	struct FrameInput
	{
		std::vector<uint8_t> colorRgba8;
		std::vector<float> depth;
		std::vector<std::array<float, 2>> motion;
		bool enableInterpolation = false;
		bool reset = false;
	};

	struct FrameOutput
	{
		uint32_t ngxResult = 0;
		std::vector<uint8_t> outputRealRgba8;
		std::vector<uint8_t> outputInterpolatedRgba8;
	};

	class Backend
	{
	public:
		Backend();
		Backend(const Backend&) = delete;
		Backend& operator=(const Backend&) = delete;
		~Backend();

		bool Initialize(const Config& config);
		bool RunFrame(const FrameInput& input, FrameOutput* output);
		void ReleaseFeature();
		void Shutdown();

		const std::string& LastError() const;
		const std::vector<std::string>& ValidationErrors() const;
		bool ValidationEnabled() const;
		bool Unsupported() const;

	private:
		struct Impl;
		Impl* impl_ = nullptr;
	};
}
