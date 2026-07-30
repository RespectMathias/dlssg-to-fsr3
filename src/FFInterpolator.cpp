#include "FFInterpolator.h"
#include "Util.h"

namespace
{
	int GetFormatPrecisionGroup(uint32_t Format)
	{
		switch (Format)
		{
		case FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
		case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT:
		case FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT:
			return 0;
		case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
		case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
			return 1;
		case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
		case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM:
		case FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
		case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM:
			return 2;
		case FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM:
			return 3;
		case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB:
		case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB:
			return 4;
		case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT:
			return 5;
		case FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
		case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM:
			return 6;
		case FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP:
			return 7;
		default:
			return -1;
		}
	}

	bool AreHUDLessFormatsCompatible(uint32_t BackBufferFormat, uint32_t HUDLessFormat)
	{
		const int backBufferGroup = GetFormatPrecisionGroup(BackBufferFormat);
		return backBufferGroup >= 0 && backBufferGroup == GetFormatPrecisionGroup(HUDLessFormat);
	}

	FrameGenerationStatus CheckApiResult(ffxReturnCode_t Result, const char *Operation)
	{
		if (Result == FFX_API_RETURN_OK)
			return FrameGenerationStatus::Success;

		spdlog::error("{} failed with FidelityFX API status {}.", Operation, Result);
		return FrameGenerationStatus::Error;
	}
}

FFInterpolator::FFInterpolator(ID3D12Device *Device, uint32_t MaxRenderWidth, uint32_t MaxRenderHeight)
	: m_Device(Device),
	  m_MaxRenderWidth(MaxRenderWidth),
	  m_MaxRenderHeight(MaxRenderHeight)
{
	const auto path = Util::GetThisDllPath() + L"amd_fidelityfx_dx12.dll";
	m_Module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!m_Module)
		throw std::runtime_error("Failed to load amd_fidelityfx_dx12.dll from DLL directory.");

	ffxLoadFunctions(&m_Functions, m_Module);
	if (!m_Functions.CreateContext || !m_Functions.DestroyContext || !m_Functions.Configure || !m_Functions.Query || !m_Functions.Dispatch)
	{
		FreeLibrary(m_Module);
		m_Module = nullptr;
		throw std::runtime_error("amd_fidelityfx_dx12.dll is missing required FidelityFX API exports.");
	}
}

FFInterpolator::~FFInterpolator()
{
	DestroyContext();
	if (m_Module)
		FreeLibrary(m_Module);
}

FrameGenerationStatus FFInterpolator::Dispatch(const FFInterpolatorDispatchParameters& Parameters)
{
	if (const auto status = CreateContextDeferred(Parameters); status != FrameGenerationStatus::Success)
		return status;

	const bool useHUDLess = m_ContextDescription.HUDLessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN;
	const auto hudLess = useHUDLess ? Parameters.InputHUDLessColorBuffer : FfxApiResource {};

	ffxConfigureDescFrameGenerationRegisterDistortionFieldResource distortion = {};
	distortion.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE;
	distortion.distortionField = Parameters.InputDistortionField;

	ffxConfigureDescFrameGeneration configure = {};
	configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	configure.header.pNext = Parameters.InputDistortionField.resource ? &distortion.header : nullptr;
	configure.frameGenerationEnabled = true;
	configure.HUDLessColor = hudLess;
	configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
	configure.flags |= Parameters.DebugTearLines ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES : 0;
	configure.flags |= Parameters.DebugView ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
	configure.generationRect = { 0, 0, static_cast<int32_t>(Parameters.OutputSize.width), static_cast<int32_t>(Parameters.OutputSize.height) };
	configure.frameID = m_FrameID;

	if (const auto status = CheckApiResult(m_Functions.Configure(&m_Context, &configure.header), "ffxConfigure");
		status != FrameGenerationStatus::Success)
		return status;

	ffxDispatchDescFrameGenerationPrepare prepare = {};
	prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prepare.frameID = m_FrameID;
	prepare.flags = configure.flags & ~FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
	prepare.commandList = Parameters.CommandList;
	prepare.renderSize = Parameters.RenderSize;
	prepare.jitterOffset = Parameters.MotionVectorJitterOffsets;
	prepare.motionVectorScale = Parameters.MotionVectorScale;
	prepare.frameTimeDelta = 1000.0f / 60.0f;
	prepare.cameraNear = Parameters.CameraNear;
	prepare.cameraFar = Parameters.CameraFar;
	prepare.cameraFovAngleVertical = Parameters.CameraFovAngleVertical;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.depth = Parameters.InputDepth;
	prepare.motionVectors = Parameters.InputMotionVectors;

	if (const auto status = CheckApiResult(m_Functions.Dispatch(&m_Context, &prepare.header), "ffxDispatch(frame generation prepare)");
		status != FrameGenerationStatus::Success)
		return status;

	ffxDispatchDescFrameGeneration dispatch = {};
	dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
	dispatch.commandList = Parameters.CommandList;
	dispatch.presentColor = Parameters.InputColorBuffer.resource ? Parameters.InputColorBuffer : hudLess;
	dispatch.outputs[0] = Parameters.OutputInterpolatedColorBuffer;
	dispatch.numGeneratedFrames = 1;
	dispatch.reset = Parameters.Reset;
	dispatch.backbufferTransferFunction = Parameters.HDR ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	dispatch.minMaxLuminance[0] = Parameters.MinMaxLuminance.x;
	dispatch.minMaxLuminance[1] = Parameters.MinMaxLuminance.y;
	dispatch.generationRect = configure.generationRect;
	dispatch.frameID = m_FrameID;

	const auto status = CheckApiResult(m_Functions.Dispatch(&m_Context, &dispatch.header), "ffxDispatch(frame generation)");
	if (status == FrameGenerationStatus::Success)
		++m_FrameID;
	return status;
}

FrameGenerationStatus FFInterpolator::CreateContextDeferred(const FFInterpolatorDispatchParameters& Parameters)
{
	ContextDescription requested = {};
	requested.Flags |= Parameters.DepthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
	requested.Flags |= Parameters.DepthPlaneInfinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0;
	requested.Flags |= Parameters.HDR ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
	requested.Flags |= Parameters.MotionVectorsFullResolution ? FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0;
	requested.Flags |= Parameters.MotionVectorJitterCancellation ? FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0;
	requested.BackBufferFormat = Parameters.InputColorBuffer.resource
		? Parameters.InputColorBuffer.description.format
		: Parameters.InputHUDLessColorBuffer.description.format;

	if (Parameters.InputHUDLessColorBuffer.resource)
	{
		const auto hudLessFormat = Parameters.InputHUDLessColorBuffer.description.format;
		if (AreHUDLessFormatsCompatible(requested.BackBufferFormat, hudLessFormat))
			requested.HUDLessFormat = hudLessFormat;
		else if (!m_HUDLessWarningLogged)
		{
			spdlog::warn("HUD-less format {} is incompatible with backbuffer format {}; using backbuffer for frame generation.", hudLessFormat, requested.BackBufferFormat);
			m_HUDLessWarningLogged = true;
		}
	}

	if (std::exchange(m_ContextFlushPending, false))
		DestroyContext();

	if (m_Context)
	{
		if (requested == m_ContextDescription)
			return FrameGenerationStatus::Success;

		m_ContextFlushPending = true;
		return FrameGenerationStatus::FlushRequired;
	}

	ffxCreateContextDescFrameGeneration create = {};
	create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	create.flags = requested.Flags;
	create.displaySize = { m_MaxRenderWidth, m_MaxRenderHeight };
	create.maxRenderSize = create.displaySize;
	create.backBufferFormat = requested.BackBufferFormat;

	ffxCreateBackendDX12Desc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = m_Device;
	create.header.pNext = &backend.header;

	ffxCreateContextDescFrameGenerationHudless hudLess = {};
	if (requested.HUDLessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN)
	{
		hudLess.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
		hudLess.hudlessBackBufferFormat = requested.HUDLessFormat;
		backend.header.pNext = &hudLess.header;
	}

	const auto result = m_Functions.CreateContext(&m_Context, &create.header, nullptr);
	if (result != FFX_API_RETURN_OK)
	{
		m_Context = nullptr;
		return CheckApiResult(result, "ffxCreateContext");
	}

	m_ContextDescription = requested;
	m_FrameID = 0;
	return FrameGenerationStatus::Success;
}

void FFInterpolator::DestroyContext()
{
	if (m_Context)
	{
		const auto result = m_Functions.DestroyContext(&m_Context, nullptr);
		if (result != FFX_API_RETURN_OK)
			spdlog::error("ffxDestroyContext failed with FidelityFX API status {}.", result);
	}
	m_Context = nullptr;
}
