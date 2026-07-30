#pragma once

#include <vulkan/vulkan.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <ffx_api/ffx_api_types.h>

#include "VulkanSubmitHooks.h"

struct NGXInstanceParameters;
class VulkanCopyPipeline;

class VulkanDX12Interop : public std::enable_shared_from_this<VulkanDX12Interop>
{
private:
	static constexpr uint32_t FrameCount = 2;

	struct SharedTexture
	{
		VkImage SourceImage = VK_NULL_HANDLE;
		VkImageView SourceView = VK_NULL_HANDLE;
		VkImageSubresourceRange SourceSubresource = {};
		VkFormat SourceFormat = VK_FORMAT_UNDEFINED;
		uint32_t Width = 0;
		uint32_t Height = 0;
		VkImage SharedImage = VK_NULL_HANDLE;
		VkImageView SharedView = VK_NULL_HANDLE;
		VkDeviceMemory SharedMemory = VK_NULL_HANDLE;
		VkFormat SharedFormat = VK_FORMAT_UNDEFINED;
		VkImageLayout SharedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		uint32_t DX12State = FFX_API_RESOURCE_STATE_COMMON;
		Microsoft::WRL::ComPtr<ID3D12Resource> DX12Resource;
		bool Output = false;
	};

	struct FrameResources
	{
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> DX12Allocator;
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> DX12CommandList;
		VkCommandPool CopyBackPool = VK_NULL_HANDLE;
		VkCommandBuffer CopyBackCommandBuffer = VK_NULL_HANDLE;
		uint64_t CopyBackValue = 0;
		uint64_t CopyInValue = 0;
		uint64_t DX12DoneValue = 0;
		bool Pending = false;
		std::vector<SharedTexture *> ActiveTextures;
		std::vector<SharedTexture *> ActiveOutputs;
	};

	VkDevice m_VulkanDevice = VK_NULL_HANDLE;
	VkPhysicalDevice m_VulkanPhysicalDevice = VK_NULL_HANDLE;
	uint32_t m_QueueFamilyIndex = UINT32_MAX;
	PFN_vkGetMemoryWin32HandlePropertiesKHR m_GetMemoryWin32HandleProperties = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR m_ImportSemaphoreWin32Handle = nullptr;
	PFN_vkWaitSemaphores m_WaitSemaphores = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Device> m_DX12Device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_DX12Queue;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_InteropFence;
	HANDLE m_InteropFenceHandle = nullptr;
	VkSemaphore m_InteropSemaphore = VK_NULL_HANDLE;
	VkSemaphore m_CopyBackSemaphore = VK_NULL_HANDLE;

	std::array<FrameResources, FrameCount> m_Frames;
	std::unordered_map<std::string, SharedTexture> m_Textures;
	std::unordered_map<std::string, bool> m_CopiedInputs;
	std::vector<SharedTexture *> m_ActiveTextures;
	std::vector<SharedTexture *> m_ActiveOutputs;
	std::unordered_map<VkCommandBuffer, uint32_t> m_PendingFrames;
	std::unique_ptr<VulkanCopyPipeline> m_CopyPipeline;

	VkCommandBuffer m_ActiveVulkanCommandBuffer = VK_NULL_HANDLE;
	ID3D12GraphicsCommandList *m_ActiveDX12CommandList = nullptr;
	uint32_t m_FrameIndex = 0;
	uint64_t m_NextInteropValue = 0;
	uint64_t m_NextCopyBackValue = 0;
	bool m_UseTransferForInputs = false;
	bool m_UseTransferForOutputs = false;
	bool m_FrameOpen = false;
	std::recursive_mutex m_Mutex;

public:
	VulkanDX12Interop(VkDevice Device, VkPhysicalDevice PhysicalDevice);
	VulkanDX12Interop(const VulkanDX12Interop&) = delete;
	VulkanDX12Interop& operator=(const VulkanDX12Interop&) = delete;
	~VulkanDX12Interop();

	ID3D12Device *GetDX12Device() const;
	std::array<uint8_t, 8> GetAdapterLUID() const;
	void *GetActiveCommandList() const;

	bool BeginFrame(VkCommandBuffer CommandBuffer);
	bool EndFrame();
	bool PrepareSubmission(VkQueue Queue, VulkanInteropSubmission *Submission);
	bool ExecuteSubmission(const VulkanInteropSubmission& Submission);
	void CommitSubmission(const VulkanInteropSubmission& Submission);
	void CancelPending(VkCommandBuffer CommandBuffer);
	void CancelSubmission(const VulkanInteropSubmission& Submission);

	bool LoadTexture(NGXInstanceParameters *Parameters, const char *Name, FfxApiResource *OutResource, uint32_t State);
	void CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source);

private:
	bool CreateDX12Device();
	bool CreateSynchronization();
	bool CreateSharedTexture(SharedTexture *Texture);
	bool CreateCopyBackCommandBuffer(FrameResources *Frame);
	bool RecordInputCopy(SharedTexture *Texture, bool Depth);
	bool RecordOutputCopies(FrameResources *Frame);
	void TransitionDX12(SharedTexture *Texture, uint32_t State);
	void ReleaseTexture(SharedTexture *Texture);
	uint32_t FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Flags) const;
	static DXGI_FORMAT ConvertFormat(VkFormat Format);
	static D3D12_RESOURCE_STATES ConvertState(uint32_t State);
	static bool IsDepthName(const char *Name);
	static bool IsOutputName(const char *Name);
};
