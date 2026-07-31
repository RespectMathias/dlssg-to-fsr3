#include "VulkanCopyPipeline.h"
#include "VulkanCopyShader.h"

VulkanCopyPipeline::~VulkanCopyPipeline()
{
	Destroy();
}

bool VulkanCopyPipeline::Initialize(VkDevice Device)
{
	m_Device = Device;

	const std::array bindings = {
		VkDescriptorSetLayoutBinding {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
	};

	const VkDescriptorSetLayoutCreateInfo layoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(bindings.size()),
		.pBindings = bindings.data(),
	};

	if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout) != VK_SUCCESS)
		return false;

	const VkPushConstantRange pushConstantRange = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.size = sizeof(VkExtent2D),
	};
	const VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_DescriptorSetLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pushConstantRange,
	};

	if (vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS)
		return false;

	const std::array poolSizes = {
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<uint32_t>(m_DescriptorSets.size()) },
		VkDescriptorPoolSize { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<uint32_t>(m_DescriptorSets.size()) },
	};

	const VkDescriptorPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = static_cast<uint32_t>(m_DescriptorSets.size()),
		.poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
		.pPoolSizes = poolSizes.data(),
	};

	if (vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS)
		return false;

	std::array<VkDescriptorSetLayout, FrameCount * CopiesPerFrame> layouts;
	layouts.fill(m_DescriptorSetLayout);

	const VkDescriptorSetAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_DescriptorPool,
		.descriptorSetCount = static_cast<uint32_t>(layouts.size()),
		.pSetLayouts = layouts.data(),
	};

	if (vkAllocateDescriptorSets(m_Device, &allocateInfo, m_DescriptorSets.data()) != VK_SUCCESS)
		return false;

	return CreatePipeline(VulkanCopyShader, sizeof(VulkanCopyShader), &m_CopyPipeline);
}

void VulkanCopyPipeline::BeginFrame(uint32_t FrameIndex)
{
	m_CopyIndices[FrameIndex % FrameCount] = 0;
}

bool VulkanCopyPipeline::Dispatch(
	VkCommandBuffer CommandBuffer,
	VkImageView Source,
	VkImageView Destination,
	VkExtent2D Extent,
	uint32_t FrameIndex)
{
	FrameIndex %= FrameCount;
	auto& copyIndex = m_CopyIndices[FrameIndex];
	if (copyIndex >= CopiesPerFrame || !Source || !Destination)
		return false;

	const auto descriptorSet = m_DescriptorSets[FrameIndex * CopiesPerFrame + copyIndex++];
	const VkDescriptorImageInfo sourceInfo = {
		.imageView = Source,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	const VkDescriptorImageInfo destinationInfo = {
		.imageView = Destination,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};
	const std::array writes = {
		VkWriteDescriptorSet {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptorSet,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = &sourceInfo,
		},
		VkWriteDescriptorSet {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptorSet,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &destinationInfo,
		},
	};

	vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_CopyPipeline);
	vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
	vkCmdPushConstants(CommandBuffer, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Extent), &Extent);
	vkCmdDispatch(CommandBuffer, (Extent.width + 15) / 16, (Extent.height + 15) / 16, 1);
	return true;
}

bool VulkanCopyPipeline::CreatePipeline(const unsigned char *Code, size_t CodeSize, VkPipeline *Pipeline)
{
	VkShaderModule module = VK_NULL_HANDLE;
	const VkShaderModuleCreateInfo moduleInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = CodeSize,
		.pCode = reinterpret_cast<const uint32_t *>(Code),
	};

	if (vkCreateShaderModule(m_Device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
		return false;

	const VkComputePipelineCreateInfo pipelineInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		},
		.layout = m_PipelineLayout,
	};

	const auto result = vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, Pipeline);
	vkDestroyShaderModule(m_Device, module, nullptr);
	return result == VK_SUCCESS;
}

void VulkanCopyPipeline::Destroy()
{
	if (!m_Device)
		return;

	if (m_CopyPipeline)
		vkDestroyPipeline(m_Device, m_CopyPipeline, nullptr);
	if (m_DescriptorPool)
		vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
	if (m_PipelineLayout)
		vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
	if (m_DescriptorSetLayout)
		vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);

	m_Device = VK_NULL_HANDLE;
}
