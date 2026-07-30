#include <ffx_api/dx12/ffx_api_dx12.h>

#include "VulkanDX12Interop.h"
#include "VulkanCopyPipeline.h"
#include "NGX/NvNGX.h"
#include "Util.h"

using Microsoft::WRL::ComPtr;

VulkanDX12Interop::VulkanDX12Interop(VkDevice Device, VkPhysicalDevice PhysicalDevice)
	: m_VulkanDevice(Device),
	  m_VulkanPhysicalDevice(PhysicalDevice)
{
	m_GetMemoryWin32HandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
		vkGetDeviceProcAddr(m_VulkanDevice, "vkGetMemoryWin32HandlePropertiesKHR"));
	m_ImportSemaphoreWin32Handle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
		vkGetDeviceProcAddr(m_VulkanDevice, "vkImportSemaphoreWin32HandleKHR"));
	m_WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(vkGetDeviceProcAddr(m_VulkanDevice, "vkWaitSemaphores"));
	VkPhysicalDeviceFeatures deviceFeatures = {};
	vkGetPhysicalDeviceFeatures(m_VulkanPhysicalDevice, &deviceFeatures);

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_VulkanPhysicalDevice, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(m_VulkanPhysicalDevice, &queueFamilyCount, queueFamilies.data());

	for (uint32_t i = 0; i < queueFamilyCount; i++)
	{
		if ((queueFamilies[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
			(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
		{
			m_QueueFamilyIndex = i;
			break;
		}
	}
	if (m_QueueFamilyIndex == UINT32_MAX)
	{
		for (uint32_t i = 0; i < queueFamilyCount; i++)
		{
			if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			{
				m_QueueFamilyIndex = i;
				break;
			}
		}
	}

	if (!deviceFeatures.shaderStorageImageWriteWithoutFormat || !m_GetMemoryWin32HandleProperties || !m_ImportSemaphoreWin32Handle ||
		!m_WaitSemaphores || m_QueueFamilyIndex == UINT32_MAX || !CreateDX12Device() || !CreateSynchronization())
	{
		throw std::runtime_error("Required Vulkan-DX12 interop capability is unavailable.");
	}

	m_CopyPipeline = std::make_unique<VulkanCopyPipeline>();
	if (!m_CopyPipeline->Initialize(m_VulkanDevice))
		throw std::runtime_error("Failed to create Vulkan interop copy pipelines.");

	m_UseTransferForInputs = Util::GetSetting(L"VulkanUseCopyForInputs", false);
	m_UseTransferForOutputs = Util::GetSetting(L"VulkanUseCopyForOutput", false);

	if (!VulkanSubmitHooks::Initialize(m_VulkanDevice))
		throw std::runtime_error("Failed to install Vulkan submission hooks.");
}

VulkanDX12Interop::~VulkanDX12Interop()
{
	VulkanSubmitHooks::Unregister(this);
	for (const auto& frame : m_Frames)
	{
		if (!frame.CopyBackValue)
			continue;

		const VkSemaphoreWaitInfo waitInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &m_CopyBackSemaphore,
			.pValues = &frame.CopyBackValue,
		};
		m_WaitSemaphores(m_VulkanDevice, &waitInfo, UINT64_MAX);
	}

	for (auto& [name, texture] : m_Textures)
		ReleaseTexture(&texture);

	for (auto& frame : m_Frames)
	{
		if (frame.CopyBackCommandBuffer && frame.CopyBackPool)
			vkFreeCommandBuffers(m_VulkanDevice, frame.CopyBackPool, 1, &frame.CopyBackCommandBuffer);
		if (frame.CopyBackPool)
			vkDestroyCommandPool(m_VulkanDevice, frame.CopyBackPool, nullptr);
	}

	m_CopyPipeline.reset();

	if (m_CopyBackSemaphore)
		vkDestroySemaphore(m_VulkanDevice, m_CopyBackSemaphore, nullptr);
	if (m_InteropSemaphore)
		vkDestroySemaphore(m_VulkanDevice, m_InteropSemaphore, nullptr);
	if (m_InteropFenceHandle)
		CloseHandle(m_InteropFenceHandle);
}

ID3D12Device *VulkanDX12Interop::GetDX12Device() const
{
	return m_DX12Device.Get();
}

std::array<uint8_t, 8> VulkanDX12Interop::GetAdapterLUID() const
{
	const auto luid = m_DX12Device->GetAdapterLuid();
	std::array<uint8_t, sizeof(luid)> result;
	memcpy(result.data(), &luid, result.size());
	return result;
}

void *VulkanDX12Interop::GetActiveCommandList() const
{
	return m_ActiveDX12CommandList;
}

bool VulkanDX12Interop::BeginFrame(VkCommandBuffer CommandBuffer)
{
	std::unique_lock lock(m_Mutex);
	if (m_FrameOpen || !CommandBuffer)
		return false;

	m_FrameIndex = (m_FrameIndex + 1) % FrameCount;
	auto& frame = m_Frames[m_FrameIndex];
	if (frame.Pending)
		return false;

	if (frame.CopyBackValue)
	{
		const auto copyBackValue = frame.CopyBackValue;
		const VkSemaphoreWaitInfo waitInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &m_CopyBackSemaphore,
			.pValues = &copyBackValue,
		};

		lock.unlock();
		if (m_WaitSemaphores(m_VulkanDevice, &waitInfo, UINT64_MAX) != VK_SUCCESS)
			return false;
		lock.lock();
		frame.CopyBackValue = 0;
	}

	if (FAILED(frame.DX12Allocator->Reset()) || FAILED(frame.DX12CommandList->Reset(frame.DX12Allocator.Get(), nullptr)))
		return false;

	m_ActiveVulkanCommandBuffer = CommandBuffer;
	m_ActiveDX12CommandList = frame.DX12CommandList.Get();
	m_ActiveTextures.clear();
	m_ActiveOutputs.clear();
	m_CopiedInputs.clear();
	m_CopyPipeline->BeginFrame(m_FrameIndex);
	m_FrameOpen = true;
	return true;
}

bool VulkanDX12Interop::EndFrame()
{
	std::scoped_lock lock(m_Mutex);
	if (!m_FrameOpen)
		return false;

	auto& frame = m_Frames[m_FrameIndex];
	for (auto *texture : m_ActiveTextures)
		TransitionDX12(texture, FFX_API_RESOURCE_STATE_COMMON);

	if (FAILED(m_ActiveDX12CommandList->Close()))
		return false;

	frame.ActiveTextures = m_ActiveTextures;
	frame.ActiveOutputs = m_ActiveOutputs;
	frame.Pending = true;
	if (const auto previous = m_PendingFrames.find(m_ActiveVulkanCommandBuffer); previous != m_PendingFrames.end())
	{
		m_Frames[previous->second].Pending = false;
		m_Frames[previous->second].CopyBackValue = 0;
		m_PendingFrames.erase(previous);
	}
	m_PendingFrames[m_ActiveVulkanCommandBuffer] = m_FrameIndex;
	VulkanSubmitHooks::RegisterPending(m_ActiveVulkanCommandBuffer, shared_from_this());

	m_ActiveDX12CommandList = nullptr;
	m_ActiveVulkanCommandBuffer = VK_NULL_HANDLE;
	m_FrameOpen = false;
	return true;
}

bool VulkanDX12Interop::PrepareSubmission(VkQueue Queue, VulkanInteropSubmission *Submission)
{
	std::scoped_lock lock(m_Mutex);
	if (!Submission || !Submission->TriggerCommandBuffer)
		return false;

	const auto pending = m_PendingFrames.find(Submission->TriggerCommandBuffer);
	if (pending == m_PendingFrames.end())
		return false;

	auto& frame = m_Frames[pending->second];
	VkQueue expectedQueue = VK_NULL_HANDLE;
	vkGetDeviceQueue(m_VulkanDevice, m_QueueFamilyIndex, 0, &expectedQueue);
	if (expectedQueue != Queue)
	{
		spdlog::error("Vulkan FG queue family cannot be determined safely.");
		return false;
	}

	frame.CopyInValue = ++m_NextInteropValue;
	frame.DX12DoneValue = ++m_NextInteropValue;

	if (!CreateCopyBackCommandBuffer(&frame) || !RecordOutputCopies(&frame))
		return false;

	frame.CopyBackValue = ++m_NextCopyBackValue;
	Submission->CopyBackCommandBuffer = frame.CopyBackCommandBuffer;
	Submission->InteropSemaphore = m_InteropSemaphore;
	Submission->CopyBackSemaphore = m_CopyBackSemaphore;
	Submission->CopyInValue = frame.CopyInValue;
	Submission->DX12DoneValue = frame.DX12DoneValue;
	Submission->CopyBackValue = frame.CopyBackValue;
	Submission->FrameIndex = pending->second;
	m_PendingFrames.erase(pending);
	return true;
}

bool VulkanDX12Interop::ExecuteSubmission(const VulkanInteropSubmission& Submission)
{
	std::scoped_lock lock(m_Mutex);
	auto& frame = m_Frames[Submission.FrameIndex % FrameCount];
	if (FAILED(m_DX12Queue->Wait(m_InteropFence.Get(), Submission.CopyInValue)))
		return false;

	ID3D12CommandList *commandLists[] = { frame.DX12CommandList.Get() };
	m_DX12Queue->ExecuteCommandLists(1, commandLists);
	return SUCCEEDED(m_DX12Queue->Signal(m_InteropFence.Get(), Submission.DX12DoneValue));
}

void VulkanDX12Interop::CommitSubmission(const VulkanInteropSubmission& Submission)
{
	std::scoped_lock lock(m_Mutex);
	m_Frames[Submission.FrameIndex % FrameCount].Pending = false;
}

void VulkanDX12Interop::CancelPending(VkCommandBuffer CommandBuffer)
{
	std::scoped_lock lock(m_Mutex);
	const auto pending = m_PendingFrames.find(CommandBuffer);
	if (pending == m_PendingFrames.end())
		return;
	m_Frames[pending->second].Pending = false;
	m_PendingFrames.erase(pending);
}

void VulkanDX12Interop::CancelSubmission(const VulkanInteropSubmission& Submission)
{
	std::scoped_lock lock(m_Mutex);
	auto& frame = m_Frames[Submission.FrameIndex % FrameCount];
	frame.Pending = false;
	frame.CopyBackValue = 0;
}

bool VulkanDX12Interop::LoadTexture(NGXInstanceParameters *Parameters, const char *Name, FfxApiResource *OutResource, uint32_t State)
{
	std::scoped_lock lock(m_Mutex);
	NGXVulkanResourceHandle *handle = nullptr;
	Parameters->GetVoidPointer(Name, reinterpret_cast<void **>(&handle));
	if (!handle || handle->Type != 0 || !handle->ImageMetadata.Image || !handle->ImageMetadata.View)
	{
		*OutResource = {};
		return false;
	}

	const auto key = fmt::format("{}:{:X}", Name, reinterpret_cast<uintptr_t>(handle->ImageMetadata.Image));
	auto& texture = m_Textures[key];
	const auto sharedFormat = IsDepthName(Name)													   ? VK_FORMAT_R32_SFLOAT
							  : handle->ImageMetadata.Format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
																								   : handle->ImageMetadata.Format;
	const bool recreate = !texture.DX12Resource || texture.Width != handle->ImageMetadata.Width ||
						  texture.Height != handle->ImageMetadata.Height || texture.SharedFormat != sharedFormat;

	if (recreate)
	{
		ReleaseTexture(&texture);
		texture.Width = handle->ImageMetadata.Width;
		texture.Height = handle->ImageMetadata.Height;
		texture.SourceFormat = handle->ImageMetadata.Format;
		texture.SharedFormat = sharedFormat;
		texture.Output = IsOutputName(Name);
		if (!CreateSharedTexture(&texture))
		{
			ReleaseTexture(&texture);
			*OutResource = {};
			return false;
		}
	}

	texture.SourceImage = handle->ImageMetadata.Image;
	texture.SourceView = handle->ImageMetadata.View;
	texture.SourceSubresource = handle->ImageMetadata.Subresource;
	texture.SourceFormat = handle->ImageMetadata.Format;

	if (!texture.Output && !m_CopiedInputs[Name])
	{
		if (!RecordInputCopy(&texture, IsDepthName(Name)))
		{
			*OutResource = {};
			return false;
		}
		m_CopiedInputs[Name] = true;
	}

	if (std::find(m_ActiveTextures.begin(), m_ActiveTextures.end(), &texture) == m_ActiveTextures.end())
		m_ActiveTextures.push_back(&texture);
	if (texture.Output && std::find(m_ActiveOutputs.begin(), m_ActiveOutputs.end(), &texture) == m_ActiveOutputs.end())
		m_ActiveOutputs.push_back(&texture);

	TransitionDX12(&texture, State);
	*OutResource = ffxApiGetResourceDX12(texture.DX12Resource.Get(), State);
	return true;
}

void VulkanDX12Interop::CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source)
{
	std::scoped_lock lock(m_Mutex);
	const auto commandList = reinterpret_cast<ID3D12GraphicsCommandList *>(CommandList);
	std::array barriers = {
		D3D12_RESOURCE_BARRIER {
			.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
			.Transition = {
				.pResource = static_cast<ID3D12Resource *>(Destination->resource),
				.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				.StateBefore = ConvertState(Destination->state),
				.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST,
			},
		},
		D3D12_RESOURCE_BARRIER {
			.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
			.Transition = {
				.pResource = static_cast<ID3D12Resource *>(Source->resource),
				.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				.StateBefore = ConvertState(Source->state),
				.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
			},
		},
	};

	commandList->ResourceBarrier(static_cast<uint32_t>(barriers.size()), barriers.data());
	commandList->CopyResource(barriers[0].Transition.pResource, barriers[1].Transition.pResource);
	for (auto& barrier : barriers)
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	commandList->ResourceBarrier(static_cast<uint32_t>(barriers.size()), barriers.data());
}

bool VulkanDX12Interop::CreateDX12Device()
{
	VkPhysicalDeviceIDProperties idProperties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &idProperties,
	};
	vkGetPhysicalDeviceProperties2(m_VulkanPhysicalDevice, &properties);
	if (!idProperties.deviceLUIDValid)
		return false;

	LUID luid = {};
	static_assert(sizeof(luid) == sizeof(idProperties.deviceLUID));
	memcpy(&luid, idProperties.deviceLUID, sizeof(luid));

	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
		FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_DX12Device))))
	{
		return false;
	}

	const D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
	if (FAILED(m_DX12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_DX12Queue))))
		return false;

	for (auto& frame : m_Frames)
	{
		if (FAILED(m_DX12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.DX12Allocator))) ||
			FAILED(m_DX12Device->CreateCommandList(
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				frame.DX12Allocator.Get(),
				nullptr,
				IID_PPV_ARGS(&frame.DX12CommandList))) ||
			FAILED(frame.DX12CommandList->Close()))
		{
			return false;
		}
	}

	return true;
}

bool VulkanDX12Interop::CreateSynchronization()
{
	if (FAILED(m_DX12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_InteropFence))) ||
		FAILED(m_DX12Device->CreateSharedHandle(m_InteropFence.Get(), nullptr, GENERIC_ALL, nullptr, &m_InteropFenceHandle)))
	{
		return false;
	}

	const VkSemaphoreTypeCreateInfo typeInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	const VkSemaphoreCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &typeInfo,
	};
	if (vkCreateSemaphore(m_VulkanDevice, &createInfo, nullptr, &m_InteropSemaphore) != VK_SUCCESS ||
		vkCreateSemaphore(m_VulkanDevice, &createInfo, nullptr, &m_CopyBackSemaphore) != VK_SUCCESS)
	{
		return false;
	}

	const VkImportSemaphoreWin32HandleInfoKHR importInfo = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
		.semaphore = m_InteropSemaphore,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
		.handle = m_InteropFenceHandle,
	};
	return m_ImportSemaphoreWin32Handle(m_VulkanDevice, &importInfo) == VK_SUCCESS;
}

bool VulkanDX12Interop::CreateSharedTexture(SharedTexture *Texture)
{
	const auto dxgiFormat = ConvertFormat(Texture->SharedFormat);
	if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
	{
		spdlog::error("Unsupported Vulkan interop format: {}", static_cast<uint32_t>(Texture->SharedFormat));
		return false;
	}

	D3D12_FEATURE_DATA_FORMAT_SUPPORT dx12FormatSupport = { .Format = dxgiFormat };
	if (FAILED(m_DX12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &dx12FormatSupport, sizeof(dx12FormatSupport))) ||
		!(dx12FormatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
		(Texture->Output && !(dx12FormatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
	{
		return false;
	}

	VkFormatProperties formatProperties = {};
	vkGetPhysicalDeviceFormatProperties(m_VulkanPhysicalDevice, Texture->SharedFormat, &formatProperties);
	const auto requiredFormatFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
	if ((formatProperties.optimalTilingFeatures & requiredFormatFeatures) != requiredFormatFeatures)
		return false;

	const VkPhysicalDeviceExternalImageFormatInfo externalFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
	};
	const VkPhysicalDeviceImageFormatInfo2 formatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &externalFormatInfo,
		.format = Texture->SharedFormat,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	VkExternalImageFormatProperties externalProperties = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 imageProperties = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &externalProperties,
	};
	if (vkGetPhysicalDeviceImageFormatProperties2(m_VulkanPhysicalDevice, &formatInfo, &imageProperties) != VK_SUCCESS ||
		!(externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
	{
		return false;
	}

	const D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_DESC resourceDesc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = Texture->Width,
		.Height = Texture->Height,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = dxgiFormat,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
				 (Texture->Output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE),
	};

	if (FAILED(m_DX12Device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_SHARED,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&Texture->DX12Resource))))
	{
		return false;
	}

	HANDLE sharedHandle = nullptr;
	if (FAILED(m_DX12Device->CreateSharedHandle(Texture->DX12Resource.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle)))
		return false;

	const VkExternalMemoryImageCreateInfo externalInfo = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
	};
	const VkImageCreateInfo imageInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &externalInfo,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = Texture->SharedFormat,
		.extent = { Texture->Width, Texture->Height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	if (vkCreateImage(m_VulkanDevice, &imageInfo, nullptr, &Texture->SharedImage) != VK_SUCCESS)
	{
		CloseHandle(sharedHandle);
		return false;
	}

	VkMemoryRequirements requirements = {};
	vkGetImageMemoryRequirements(m_VulkanDevice, Texture->SharedImage, &requirements);
	VkMemoryWin32HandlePropertiesKHR handleProperties = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR,
	};
	if (m_GetMemoryWin32HandleProperties(
			m_VulkanDevice,
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
			sharedHandle,
			&handleProperties) != VK_SUCCESS)
	{
		CloseHandle(sharedHandle);
		return false;
	}

	const VkMemoryDedicatedAllocateInfo dedicatedInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = Texture->SharedImage,
	};
	const VkImportMemoryWin32HandleInfoKHR importInfo = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
		.pNext = &dedicatedInfo,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
		.handle = sharedHandle,
	};
	const VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &importInfo,
		.allocationSize = requirements.size,
		.memoryTypeIndex = FindMemoryType(
			requirements.memoryTypeBits & handleProperties.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};

	if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
		vkAllocateMemory(m_VulkanDevice, &allocateInfo, nullptr, &Texture->SharedMemory) != VK_SUCCESS ||
		vkBindImageMemory(m_VulkanDevice, Texture->SharedImage, Texture->SharedMemory, 0) != VK_SUCCESS)
	{
		CloseHandle(sharedHandle);
		return false;
	}
	CloseHandle(sharedHandle);

	const VkImageViewCreateInfo viewInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = Texture->SharedImage,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = Texture->SharedFormat,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	return vkCreateImageView(m_VulkanDevice, &viewInfo, nullptr, &Texture->SharedView) == VK_SUCCESS;
}

bool VulkanDX12Interop::CreateCopyBackCommandBuffer(FrameResources *Frame)
{
	if (!Frame->CopyBackPool)
	{
		const VkCommandPoolCreateInfo poolInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = m_QueueFamilyIndex,
		};
		if (vkCreateCommandPool(m_VulkanDevice, &poolInfo, nullptr, &Frame->CopyBackPool) != VK_SUCCESS)
			return false;

		const VkCommandBufferAllocateInfo allocateInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = Frame->CopyBackPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		if (vkAllocateCommandBuffers(m_VulkanDevice, &allocateInfo, &Frame->CopyBackCommandBuffer) != VK_SUCCESS)
			return false;
	}

	return vkResetCommandBuffer(Frame->CopyBackCommandBuffer, 0) == VK_SUCCESS;
}

bool VulkanDX12Interop::RecordInputCopy(SharedTexture *Texture, bool Depth)
{
	const VkImageAspectFlags sourceAspect = Depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageSubresourceRange sourceRange = Texture->SourceSubresource;
	sourceRange.aspectMask = sourceAspect;
	sourceRange.levelCount = 1;
	sourceRange.layerCount = 1;
	const VkImageSubresourceRange sharedRange = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1,
	};

	if (m_UseTransferForInputs && !Depth && Texture->SourceFormat == Texture->SharedFormat)
	{
		std::array barriers = {
			VkImageMemoryBarrier {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = Texture->SourceImage,
				.subresourceRange = sourceRange,
			},
			VkImageMemoryBarrier {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.oldLayout = Texture->SharedLayout,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
				.dstQueueFamilyIndex = m_QueueFamilyIndex,
				.image = Texture->SharedImage,
				.subresourceRange = sharedRange,
			},
		};
		vkCmdPipelineBarrier(
			m_ActiveVulkanCommandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0,
			nullptr,
			0,
			nullptr,
			static_cast<uint32_t>(barriers.size()),
			barriers.data());

		const VkImageCopy copy = {
			.srcSubresource = { sourceAspect, sourceRange.baseMipLevel, sourceRange.baseArrayLayer, 1 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.extent = { Texture->Width, Texture->Height, 1 },
		};
		vkCmdCopyImage(
			m_ActiveVulkanCommandBuffer,
			Texture->SourceImage,
			barriers[0].newLayout,
			Texture->SharedImage,
			barriers[1].newLayout,
			1,
			&copy);

		for (auto& barrier : barriers)
		{
			barrier.srcAccessMask = barrier.dstAccessMask;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			std::swap(barrier.oldLayout, barrier.newLayout);
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		}
		barriers[1].srcQueueFamilyIndex = m_QueueFamilyIndex;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		vkCmdPipelineBarrier(
			m_ActiveVulkanCommandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0,
			0,
			nullptr,
			0,
			nullptr,
			static_cast<uint32_t>(barriers.size()),
			barriers.data());
	}
	else
	{
		std::array barriers = {
			VkImageMemoryBarrier {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = Texture->SourceImage,
				.subresourceRange = sourceRange,
			},
			VkImageMemoryBarrier {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
				.oldLayout = Texture->SharedLayout,
				.newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
				.dstQueueFamilyIndex = m_QueueFamilyIndex,
				.image = Texture->SharedImage,
				.subresourceRange = sharedRange,
			},
		};
		vkCmdPipelineBarrier(
			m_ActiveVulkanCommandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0,
			0,
			nullptr,
			0,
			nullptr,
			static_cast<uint32_t>(barriers.size()),
			barriers.data());

		if (!m_CopyPipeline->Dispatch(
				m_ActiveVulkanCommandBuffer,
				Texture->SourceView,
				Texture->SharedView,
				{ Texture->Width, Texture->Height },
				m_FrameIndex))
			return false;

		barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[1].srcQueueFamilyIndex = m_QueueFamilyIndex;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		vkCmdPipelineBarrier(
			m_ActiveVulkanCommandBuffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0,
			0,
			nullptr,
			0,
			nullptr,
			static_cast<uint32_t>(barriers.size()),
			barriers.data());
	}

	Texture->SharedLayout = VK_IMAGE_LAYOUT_GENERAL;
	return true;
}

bool VulkanDX12Interop::RecordOutputCopies(FrameResources *Frame)
{
	const VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (vkBeginCommandBuffer(Frame->CopyBackCommandBuffer, &beginInfo) != VK_SUCCESS)
		return false;

	const auto frameIndex = static_cast<uint32_t>(Frame - m_Frames.data());
	for (auto *texture : Frame->ActiveOutputs)
	{
		const VkImageSubresourceRange sharedRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		};
		auto outputRange = texture->SourceSubresource;
		outputRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		outputRange.levelCount = 1;
		outputRange.layerCount = 1;

		if (m_UseTransferForOutputs && texture->SourceFormat == texture->SharedFormat)
		{
			std::array barriers = {
				VkImageMemoryBarrier {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
					.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
					.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
					.dstQueueFamilyIndex = m_QueueFamilyIndex,
					.image = texture->SharedImage,
					.subresourceRange = sharedRange,
				},
				VkImageMemoryBarrier {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
					.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
					.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = texture->SourceImage,
					.subresourceRange = outputRange,
				},
			};
			vkCmdPipelineBarrier(
				Frame->CopyBackCommandBuffer,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				static_cast<uint32_t>(barriers.size()),
				barriers.data());

			const VkImageCopy copy = {
				.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.dstSubresource = {
					VK_IMAGE_ASPECT_COLOR_BIT,
					outputRange.baseMipLevel,
					outputRange.baseArrayLayer,
					1,
				},
				.extent = { texture->Width, texture->Height, 1 },
			};
			vkCmdCopyImage(
				Frame->CopyBackCommandBuffer,
				texture->SharedImage,
				barriers[0].newLayout,
				texture->SourceImage,
				barriers[1].newLayout,
				1,
				&copy);

			for (auto& barrier : barriers)
			{
				barrier.srcAccessMask = barrier.dstAccessMask;
				barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				barrier.oldLayout = barrier.newLayout;
				barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			}
			barriers[0].srcQueueFamilyIndex = m_QueueFamilyIndex;
			barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			vkCmdPipelineBarrier(
				Frame->CopyBackCommandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				static_cast<uint32_t>(barriers.size()),
				barriers.data());
		}
		else
		{
			const VkImageMemoryBarrier sharedBarrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
				.dstQueueFamilyIndex = m_QueueFamilyIndex,
				.image = texture->SharedImage,
				.subresourceRange = sharedRange,
			};
			vkCmdPipelineBarrier(
				Frame->CopyBackCommandBuffer,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				1,
				&sharedBarrier);
			if (!m_CopyPipeline->Dispatch(
					Frame->CopyBackCommandBuffer,
					texture->SharedView,
					texture->SourceView,
					{ texture->Width, texture->Height },
					frameIndex))
				return false;

			auto restore = sharedBarrier;
			restore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			restore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			restore.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			restore.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			restore.srcQueueFamilyIndex = m_QueueFamilyIndex;
			restore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			vkCmdPipelineBarrier(
				Frame->CopyBackCommandBuffer,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				1,
				&restore);
		}
	}

	return vkEndCommandBuffer(Frame->CopyBackCommandBuffer) == VK_SUCCESS;
}

void VulkanDX12Interop::TransitionDX12(SharedTexture *Texture, uint32_t State)
{
	if (Texture->DX12State == State)
		return;

	const D3D12_RESOURCE_BARRIER barrier = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = {
			.pResource = Texture->DX12Resource.Get(),
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = ConvertState(Texture->DX12State),
			.StateAfter = ConvertState(State),
		},
	};
	m_ActiveDX12CommandList->ResourceBarrier(1, &barrier);
	Texture->DX12State = State;
}

void VulkanDX12Interop::ReleaseTexture(SharedTexture *Texture)
{
	if (Texture->SharedView)
		vkDestroyImageView(m_VulkanDevice, Texture->SharedView, nullptr);
	if (Texture->SharedImage)
		vkDestroyImage(m_VulkanDevice, Texture->SharedImage, nullptr);
	if (Texture->SharedMemory)
		vkFreeMemory(m_VulkanDevice, Texture->SharedMemory, nullptr);
	*Texture = {};
}

uint32_t VulkanDX12Interop::FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Flags) const
{
	VkPhysicalDeviceMemoryProperties properties = {};
	vkGetPhysicalDeviceMemoryProperties(m_VulkanPhysicalDevice, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
	{
		if ((TypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & Flags) == Flags)
			return i;
	}
	return UINT32_MAX;
}

DXGI_FORMAT VulkanDX12Interop::ConvertFormat(VkFormat Format)
{
	switch (Format)
	{
	case VK_FORMAT_R8_UNORM:
		return DXGI_FORMAT_R8_UNORM;
	case VK_FORMAT_R8G8_UNORM:
		return DXGI_FORMAT_R8G8_UNORM;
	case VK_FORMAT_R8G8_SNORM:
		return DXGI_FORMAT_R8G8_SNORM;
	case VK_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_R8G8B8A8_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case VK_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		return DXGI_FORMAT_R11G11B10_FLOAT;
	case VK_FORMAT_R16_SFLOAT:
		return DXGI_FORMAT_R16_FLOAT;
	case VK_FORMAT_R16_UNORM:
		return DXGI_FORMAT_R16_UNORM;
	case VK_FORMAT_R16_SNORM:
		return DXGI_FORMAT_R16_SNORM;
	case VK_FORMAT_R16G16_SFLOAT:
		return DXGI_FORMAT_R16G16_FLOAT;
	case VK_FORMAT_R16G16_UNORM:
		return DXGI_FORMAT_R16G16_UNORM;
	case VK_FORMAT_R16G16_SNORM:
		return DXGI_FORMAT_R16G16_SNORM;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case VK_FORMAT_R16G16B16A16_UNORM:
		return DXGI_FORMAT_R16G16B16A16_UNORM;
	case VK_FORMAT_R16G16B16A16_SNORM:
		return DXGI_FORMAT_R16G16B16A16_SNORM;
	case VK_FORMAT_R32_SFLOAT:
		return DXGI_FORMAT_R32_FLOAT;
	case VK_FORMAT_R32G32_SFLOAT:
		return DXGI_FORMAT_R32G32_FLOAT;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

D3D12_RESOURCE_STATES VulkanDX12Interop::ConvertState(uint32_t State)
{
	switch (State)
	{
	case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	case FFX_API_RESOURCE_STATE_COMPUTE_READ:
		return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	case FFX_API_RESOURCE_STATE_COPY_SRC:
		return D3D12_RESOURCE_STATE_COPY_SOURCE;
	case FFX_API_RESOURCE_STATE_COPY_DEST:
		return D3D12_RESOURCE_STATE_COPY_DEST;
	case FFX_API_RESOURCE_STATE_PRESENT:
		return D3D12_RESOURCE_STATE_PRESENT;
	case FFX_API_RESOURCE_STATE_COMMON:
		return D3D12_RESOURCE_STATE_COMMON;
	default:
		return D3D12_RESOURCE_STATE_COMMON;
	}
}

bool VulkanDX12Interop::IsDepthName(const char *Name)
{
	return strcmp(Name, "DLSSG.Depth") == 0;
}

bool VulkanDX12Interop::IsOutputName(const char *Name)
{
	return strcmp(Name, "DLSSG.OutputReal") == 0 || strcmp(Name, "DLSSG.OutputInterpolated") == 0;
}
