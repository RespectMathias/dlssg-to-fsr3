#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

enum class FrameGenerationStatus
{
	Success,
	FlushRequired,
	Error,
};

struct FFInterpolatorDispatchParameters
{
	void *CommandList = nullptr;

	FfxApiDimensions2D RenderSize = {};
	FfxApiDimensions2D OutputSize = {};

	FfxApiResource InputColorBuffer = {};
	FfxApiResource InputHUDLessColorBuffer = {};
	FfxApiResource InputDepth = {};
	FfxApiResource InputMotionVectors = {};
	FfxApiResource InputDistortionField = {};
	FfxApiResource OutputInterpolatedColorBuffer = {};

	bool MotionVectorsFullResolution = false;
	bool MotionVectorJitterCancellation = false;

	FfxApiFloatCoords2D MotionVectorScale = {};
	FfxApiFloatCoords2D MotionVectorJitterOffsets = {};

	bool HDR = false;
	bool DepthInverted = false;
	bool DepthPlaneInfinite = false;
	bool Reset = false;
	bool DebugTearLines = false;
	bool DebugView = false;

	float CameraNear = 0.0f;
	float CameraFar = 0.0f;
	float CameraFovAngleVertical = 0.0f;
	FfxApiFloatCoords2D MinMaxLuminance = {};
};

class FFInterpolator
{
private:
	struct ContextDescription
	{
		uint32_t Flags = 0;
		uint32_t BackBufferFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
		uint32_t HUDLessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;

		bool operator==(const ContextDescription&) const = default;
	};

	ID3D12Device *const m_Device;
	const uint32_t m_MaxRenderWidth;
	const uint32_t m_MaxRenderHeight;

	HMODULE m_Module = nullptr;
	ffxFunctions m_Functions = {};
	ffxContext m_Context = nullptr;
	ContextDescription m_ContextDescription = {};
	bool m_ContextFlushPending = false;
	bool m_HUDLessWarningLogged = false;
	uint64_t m_FrameID = 0;

public:
	FFInterpolator(ID3D12Device *Device, uint32_t MaxRenderWidth, uint32_t MaxRenderHeight);
	FFInterpolator(const FFInterpolator&) = delete;
	FFInterpolator& operator=(const FFInterpolator&) = delete;
	~FFInterpolator();

	FrameGenerationStatus Dispatch(const FFInterpolatorDispatchParameters& Parameters);

private:
	FrameGenerationStatus CreateContextDeferred(const FFInterpolatorDispatchParameters& Parameters);
	void DestroyContext();
};
