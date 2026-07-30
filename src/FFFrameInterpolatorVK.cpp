#include "NGX/NvNGX.h"
#include "FFFrameInterpolatorVK.h"
#include "VulkanDX12Interop.h"

FFFrameInterpolatorVK::FFFrameInterpolatorVK(
	VkDevice LogicalDevice,
	VkPhysicalDevice PhysicalDevice,
	uint32_t OutputWidth,
	uint32_t OutputHeight,
	NGXInstanceParameters *NGXParameters)
	: FFFrameInterpolator(OutputWidth, OutputHeight),
	  m_Interop(std::make_shared<VulkanDX12Interop>(LogicalDevice, PhysicalDevice))
{
	FFFrameInterpolator::Create(m_Interop->GetDX12Device(), nullptr);
}

FFFrameInterpolatorVK::~FFFrameInterpolatorVK()
{
	FFFrameInterpolator::Destroy();
	VulkanSubmitHooks::Unregister(m_Interop.get());
}

FrameGenerationStatus FFFrameInterpolatorVK::Dispatch(void *CommandList, NGXInstanceParameters *NGXParameters)
{
	NGXParameters->Set4("DLSSG.FlushRequired", 0);
	if (!m_Interop->BeginFrame(static_cast<VkCommandBuffer>(CommandList)))
		return FrameGenerationStatus::Error;

	const auto result = FFFrameInterpolator::Dispatch(nullptr, NGXParameters);
	if (!m_Interop->EndFrame())
		return FrameGenerationStatus::Error;

	return result;
}

void *FFFrameInterpolatorVK::GetActiveCommandList() const
{
	return m_Interop->GetActiveCommandList();
}

std::array<uint8_t, 8> FFFrameInterpolatorVK::GetActiveAdapterLUID() const
{
	return m_Interop->GetAdapterLUID();
}

void FFFrameInterpolatorVK::CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source)
{
	m_Interop->CopyTexture(CommandList, Destination, Source);
}

bool FFFrameInterpolatorVK::LoadTextureFromNGXParameters(
	NGXInstanceParameters *NGXParameters,
	const char *Name,
	FfxApiResource *OutFfxResource,
	uint32_t State)
{
	return m_Interop->LoadTexture(NGXParameters, Name, OutFfxResource, State);
}
