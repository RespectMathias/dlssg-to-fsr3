#pragma once

#include <vulkan/vulkan.h>

class VulkanDX12Interop;

struct VulkanInteropSubmission
{
	VkCommandBuffer TriggerCommandBuffer = VK_NULL_HANDLE;
	VkCommandBuffer CopyBackCommandBuffer = VK_NULL_HANDLE;
	VkSemaphore InteropSemaphore = VK_NULL_HANDLE;
	VkSemaphore CopyBackSemaphore = VK_NULL_HANDLE;
	uint64_t CopyInValue = 0;
	uint64_t DX12DoneValue = 0;
	uint64_t CopyBackValue = 0;
	uint32_t FrameIndex = 0;
};

namespace VulkanSubmitHooks
{
	bool Initialize(VkDevice Device);
	void RegisterPending(VkCommandBuffer CommandBuffer, std::shared_ptr<VulkanDX12Interop> Interop);
	void Unregister(VulkanDX12Interop *Interop);
}
