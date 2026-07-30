#pragma once

#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>
#include "FFFrameInterpolator.h"

class VulkanDX12Interop;

class FFFrameInterpolatorVK final : public FFFrameInterpolator
{
private:
	std::shared_ptr<VulkanDX12Interop> m_Interop;

public:
	FFFrameInterpolatorVK(
		VkDevice LogicalDevice,
		VkPhysicalDevice PhysicalDevice,
		uint32_t OutputWidth,
		uint32_t OutputHeight,
		NGXInstanceParameters *NGXParameters);
	FFFrameInterpolatorVK(const FFFrameInterpolatorVK&) = delete;
	FFFrameInterpolatorVK& operator=(const FFFrameInterpolatorVK&) = delete;
	~FFFrameInterpolatorVK();

	FrameGenerationStatus Dispatch(void *CommandList, NGXInstanceParameters *NGXParameters) override;

private:
	std::array<uint8_t, 8> GetActiveAdapterLUID() const override;
	void *GetActiveCommandList() const override;

	void CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source) override;

	bool LoadTextureFromNGXParameters(
		NGXInstanceParameters *NGXParameters,
		const char *Name,
		FfxApiResource *OutFfxResource,
		uint32_t State) override;
};
