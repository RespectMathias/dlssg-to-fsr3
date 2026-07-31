#include "NGX/NvNGX.h"
#include "FFFrameInterpolatorDX.h"

namespace
{
	D3D12_RESOURCE_STATES ConvertState(uint32_t State)
	{
		switch (State)
		{
		case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case FFX_API_RESOURCE_STATE_COMPUTE_READ:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case FFX_API_RESOURCE_STATE_PIXEL_READ:
			return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case FFX_API_RESOURCE_STATE_COPY_SRC:
			return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case FFX_API_RESOURCE_STATE_COPY_DEST:
			return D3D12_RESOURCE_STATE_COPY_DEST;
		case FFX_API_RESOURCE_STATE_GENERIC_READ:
			return D3D12_RESOURCE_STATE_GENERIC_READ;
		case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
			return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
		case FFX_API_RESOURCE_STATE_PRESENT:
		case FFX_API_RESOURCE_STATE_COMMON:
			return D3D12_RESOURCE_STATE_COMMON;
		case FFX_API_RESOURCE_STATE_RENDER_TARGET:
			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		default:
			return D3D12_RESOURCE_STATE_COMMON;
		}
	}
}

FFFrameInterpolatorDX::FFFrameInterpolatorDX(
	ID3D12Device *Device,
	uint32_t OutputWidth,
	uint32_t OutputHeight,
	NGXInstanceParameters *NGXParameters)
	: m_Device(Device),
	  FFFrameInterpolator(OutputWidth, OutputHeight)
{
	FFFrameInterpolator::Create(m_Device, NGXParameters);
	m_Device->AddRef();
}

FFFrameInterpolatorDX::~FFFrameInterpolatorDX()
{
	FFFrameInterpolator::Destroy();
	m_Device->Release();
}

FrameGenerationStatus FFFrameInterpolatorDX::Dispatch(void *CommandList, NGXInstanceParameters *NGXParameters)
{
	const bool isRecordingCommands = NGXParameters->GetUIntOrDefault("DLSSG.IsRecording", 0) != 0;
	const auto cmdList12 = reinterpret_cast<ID3D12GraphicsCommandList *>(CommandList);

	NGXParameters->Set4("DLSSG.FlushRequired", 0);

	// Begin a new command list in the event our caller didn't set one up
	if (!isRecordingCommands)
	{
		ID3D12CommandQueue *recordingQueue = nullptr;
		NGXParameters->GetVoidPointer("DLSSG.CmdQueue", reinterpret_cast<void **>(&recordingQueue));

		ID3D12CommandAllocator *recordingAllocator = nullptr;
		NGXParameters->GetVoidPointer("DLSSG.CmdAlloc", reinterpret_cast<void **>(&recordingAllocator));

		cmdList12->Reset(recordingAllocator, nullptr);
	}

	m_ActiveCommandList = cmdList12;
	const auto interpolationResult = FFFrameInterpolator::Dispatch(nullptr, NGXParameters);

	// Finish what we started. Restore the command list to its previous state when necessary.
	if (!isRecordingCommands)
		cmdList12->Close();

	return interpolationResult;
}

void *FFFrameInterpolatorDX::GetActiveCommandList() const
{
	return m_ActiveCommandList;
}

std::array<uint8_t, 8> FFFrameInterpolatorDX::GetActiveAdapterLUID() const
{
	const auto luid = m_Device->GetAdapterLuid();

	std::array<uint8_t, sizeof(luid)> result;
	memcpy(result.data(), &luid, result.size());

	return result;
}

void FFFrameInterpolatorDX::CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source)
{
	const auto cmdList12 = reinterpret_cast<ID3D12GraphicsCommandList *>(CommandList);

	D3D12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barriers[0].Transition.pResource = static_cast<ID3D12Resource *>(Destination->resource); // Destination
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[0].Transition.StateBefore = ConvertState(Destination->state);
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

	barriers[1] = barriers[0];
	barriers[1].Transition.pResource = static_cast<ID3D12Resource *>(Source->resource); // Source
	barriers[1].Transition.StateBefore = ConvertState(Source->state);
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

	cmdList12->ResourceBarrier(2, barriers);
	cmdList12->CopyResource(barriers[0].Transition.pResource, barriers[1].Transition.pResource);
	std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
	std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
	cmdList12->ResourceBarrier(2, barriers);
}

bool FFFrameInterpolatorDX::LoadTextureFromNGXParameters(
	NGXInstanceParameters *NGXParameters,
	const char *Name,
	FfxApiResource *OutFfxResource,
	uint32_t State)
{
	ID3D12Resource *resource = nullptr;
	NGXParameters->GetVoidPointer(Name, reinterpret_cast<void **>(&resource));

	if (!resource)
	{
		*OutFfxResource = {};
		return false;
	}

	*OutFfxResource = ffxApiGetResourceDX12(resource, State);
	return true;
}
