#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dx12_ngx_e2e
{
	enum class RecordingPath
	{
		callerOwned,
		dllManaged,
	};

	struct BackendConfig
	{
		std::wstring dll;
		std::uint32_t width = 128;
		std::uint32_t height = 72;
		std::chrono::milliseconds fenceTimeout = std::chrono::seconds(30);
		bool allowWarp = false;
		bool provideRejectingResourceCallbacks = false;
		bool motionVectorsDilated = false;
		bool provideIncompatibleHudless = false;

		static BackendConfig FromEnvironment();
	};

	struct FrameTimings
	{
		std::chrono::microseconds upload {};
		std::chrono::microseconds evaluateCall {};
		std::chrono::microseconds gpuCompletion {};
		std::chrono::microseconds readback {};
		std::chrono::microseconds total {};
	};

	struct FrameInput
	{
		std::vector<std::uint8_t> colorRgba8;
		std::vector<float> depth;
		std::vector<std::uint16_t> motionRg16f;
	};

	struct FrameResult
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::vector<std::uint8_t> submittedColor;
		std::vector<std::uint8_t> outputReal;
		std::vector<std::uint8_t> outputInterpolated;
		std::vector<std::uint8_t> interpolationBaseline;
		FrameTimings timings;
	};

	class Dx12NgxBackend
	{
	public:
		static std::unique_ptr<Dx12NgxBackend> Create(const BackendConfig& config);

		~Dx12NgxBackend();
		Dx12NgxBackend(const Dx12NgxBackend&) = delete;
		Dx12NgxBackend& operator=(const Dx12NgxBackend&) = delete;

		FrameResult RunFrame(
			std::uint32_t frameIndex,
			bool enableInterpolation,
			bool reset,
			RecordingPath recordingPath = RecordingPath::callerOwned);
		FrameResult RunFrame(
			const FrameInput& input,
			bool enableInterpolation,
			bool reset,
			RecordingPath recordingPath = RecordingPath::callerOwned);
		FrameResult WarmupInterpolation(std::uint32_t frameIndex = 0, RecordingPath recordingPath = RecordingPath::callerOwned);

		const std::vector<FrameTimings>& TimingHistory() const noexcept;

	private:
		class Impl;

		explicit Dx12NgxBackend(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> impl_;
	};
}
