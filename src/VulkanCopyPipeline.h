#pragma once

#include <vulkan/vulkan.h>

class VulkanCopyPipeline
{
private:
	static constexpr uint32_t FrameCount = 2;
	static constexpr uint32_t CopiesPerFrame = 12;

	VkDevice m_Device = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_CopyPipeline = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, FrameCount * CopiesPerFrame> m_DescriptorSets = {};
	std::array<uint32_t, FrameCount> m_CopyIndices = {};

public:
	VulkanCopyPipeline() = default;
	VulkanCopyPipeline(const VulkanCopyPipeline&) = delete;
	VulkanCopyPipeline& operator=(const VulkanCopyPipeline&) = delete;
	~VulkanCopyPipeline();

	bool Initialize(VkDevice Device);
	void BeginFrame(uint32_t FrameIndex);
	bool Dispatch(VkCommandBuffer CommandBuffer, VkImageView Source, VkImageView Destination, VkExtent2D Extent, uint32_t FrameIndex);

private:
	bool CreatePipeline(const unsigned char *Code, size_t CodeSize, VkPipeline *Pipeline);
	void Destroy();
};
