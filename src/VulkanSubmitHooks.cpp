#include <Windows.h>
#include <detours/detours.h>

#include "VulkanSubmitHooks.h"
#include "VulkanDX12Interop.h"

namespace
{
	std::mutex g_Mutex;
	std::unordered_map<VkCommandBuffer, std::shared_ptr<VulkanDX12Interop>> g_Pending;
	bool g_Initialized = false;

	PFN_vkQueueSubmit g_QueueSubmit = nullptr;
	PFN_vkQueueSubmit2 g_QueueSubmit2 = nullptr;
	PFN_vkQueueSubmit2KHR g_QueueSubmit2KHR = nullptr;

	const VkTimelineSemaphoreSubmitInfo *FindTimelineInfo(const void *Next)
	{
		auto current = static_cast<const VkBaseInStructure *>(Next);
		while (current)
		{
			if (current->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
				return reinterpret_cast<const VkTimelineSemaphoreSubmitInfo *>(current);
			current = current->pNext;
		}
		return nullptr;
	}

	bool HasUnsupportedLegacyInfo(const void *Next)
	{
		auto current = static_cast<const VkBaseInStructure *>(Next);
		while (current)
		{
			if (current->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
				return true;
			current = current->pNext;
		}
		return false;
	}

	std::shared_ptr<VulkanDX12Interop> TakePending(VkCommandBuffer CommandBuffer)
	{
		std::scoped_lock lock(g_Mutex);
		const auto found = g_Pending.find(CommandBuffer);
		if (found == g_Pending.end())
			return nullptr;

		auto interop = found->second;
		g_Pending.erase(found);
		return interop;
	}

	VkResult InjectLegacy(PFN_vkQueueSubmit Original, VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo *Submits, VkFence Fence)
	{
		for (uint32_t submitIndex = 0; submitIndex < SubmitCount; submitIndex++)
		{
			for (uint32_t commandIndex = 0; commandIndex < Submits[submitIndex].commandBufferCount; commandIndex++)
			{
				const auto trigger = Submits[submitIndex].pCommandBuffers[commandIndex];
				auto interop = TakePending(trigger);
				if (!interop)
					continue;
				if (HasUnsupportedLegacyInfo(Submits[submitIndex].pNext))
				{
					spdlog::error("Unsupported Vulkan submit extension chain for frame generation.");
					interop->CancelPending(trigger);
					return Original(Queue, SubmitCount, Submits, Fence);
				}

				VulkanInteropSubmission submission = { .TriggerCommandBuffer = trigger };
				if (!interop->PrepareSubmission(Queue, &submission))
				{
					spdlog::error("Failed to prepare Vulkan-DX12 frame submission.");
					interop->CancelPending(trigger);
					return Original(Queue, SubmitCount, Submits, Fence);
				}

				const auto& target = Submits[submitIndex];
				const auto *originalTimeline = FindTimelineInfo(target.pNext);
				std::vector<uint64_t> originalWaitValues(target.waitSemaphoreCount, 0);
				std::vector<uint64_t> originalSignalValues(target.signalSemaphoreCount, 0);
				if (originalTimeline)
				{
					for (uint32_t i = 0; i < std::min(target.waitSemaphoreCount, originalTimeline->waitSemaphoreValueCount); i++)
						originalWaitValues[i] = originalTimeline->pWaitSemaphoreValues[i];
					for (uint32_t i = 0; i < std::min(target.signalSemaphoreCount, originalTimeline->signalSemaphoreValueCount); i++)
						originalSignalValues[i] = originalTimeline->pSignalSemaphoreValues[i];
				}

				std::vector<VkCommandBuffer> beforeCommands(target.pCommandBuffers, target.pCommandBuffers + commandIndex + 1);
				std::vector<VkCommandBuffer> afterCommands(
					target.pCommandBuffers + commandIndex + 1,
					target.pCommandBuffers + target.commandBufferCount);

				VkTimelineSemaphoreSubmitInfo beforeTimeline = {
					.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
					.waitSemaphoreValueCount = static_cast<uint32_t>(originalWaitValues.size()),
					.pWaitSemaphoreValues = originalWaitValues.data(),
					.signalSemaphoreValueCount = 1,
					.pSignalSemaphoreValues = &submission.CopyInValue,
				};
				VkSubmitInfo before = target;
				before.pNext = &beforeTimeline;
				before.commandBufferCount = static_cast<uint32_t>(beforeCommands.size());
				before.pCommandBuffers = beforeCommands.data();
				before.signalSemaphoreCount = 1;
				before.pSignalSemaphores = &submission.InteropSemaphore;

				const VkPipelineStageFlags copyBackWaitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				const VkTimelineSemaphoreSubmitInfo copyBackTimeline = {
					.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
					.waitSemaphoreValueCount = 1,
					.pWaitSemaphoreValues = &submission.DX12DoneValue,
					.signalSemaphoreValueCount = 1,
					.pSignalSemaphoreValues = &submission.CopyBackValue,
				};
				const VkSubmitInfo copyBack = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					.pNext = &copyBackTimeline,
					.waitSemaphoreCount = 1,
					.pWaitSemaphores = &submission.InteropSemaphore,
					.pWaitDstStageMask = &copyBackWaitStage,
					.commandBufferCount = 1,
					.pCommandBuffers = &submission.CopyBackCommandBuffer,
					.signalSemaphoreCount = 1,
					.pSignalSemaphores = &submission.CopyBackSemaphore,
				};

				const VkPipelineStageFlags finalWaitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				const VkTimelineSemaphoreSubmitInfo finalTimeline = {
					.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
					.waitSemaphoreValueCount = 1,
					.pWaitSemaphoreValues = &submission.CopyBackValue,
					.signalSemaphoreValueCount = static_cast<uint32_t>(originalSignalValues.size()),
					.pSignalSemaphoreValues = originalSignalValues.data(),
				};
				const VkSubmitInfo final = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					.pNext = &finalTimeline,
					.waitSemaphoreCount = 1,
					.pWaitSemaphores = &submission.CopyBackSemaphore,
					.pWaitDstStageMask = &finalWaitStage,
					.commandBufferCount = static_cast<uint32_t>(afterCommands.size()),
					.pCommandBuffers = afterCommands.data(),
					.signalSemaphoreCount = target.signalSemaphoreCount,
					.pSignalSemaphores = target.pSignalSemaphores,
				};

				std::vector<VkSubmitInfo> injected;
				injected.reserve(SubmitCount + 2);
				for (uint32_t i = 0; i < SubmitCount; i++)
				{
					if (i == submitIndex)
					{
						injected.push_back(before);
						injected.push_back(copyBack);
						injected.push_back(final);
					}
					else
					{
						injected.push_back(Submits[i]);
					}
				}

				if (!interop->ExecuteSubmission(submission))
				{
					spdlog::error("Failed to queue DX12 frame-generation work.");
					interop->CancelSubmission(submission);
					return Original(Queue, SubmitCount, Submits, Fence);
				}

				const auto result = Original(Queue, static_cast<uint32_t>(injected.size()), injected.data(), Fence);
				if (result == VK_SUCCESS)
					interop->CommitSubmission(submission);
				else
				{
					interop->CancelSubmission(submission);
				}
				return result;
			}
		}

		return Original(Queue, SubmitCount, Submits, Fence);
	}

	template<typename SubmitFunction>
	VkResult InjectSubmit2(SubmitFunction Original, VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo2 *Submits, VkFence Fence)
	{
		for (uint32_t submitIndex = 0; submitIndex < SubmitCount; submitIndex++)
		{
			for (uint32_t commandIndex = 0; commandIndex < Submits[submitIndex].commandBufferInfoCount; commandIndex++)
			{
				const auto trigger = Submits[submitIndex].pCommandBufferInfos[commandIndex].commandBuffer;
				auto interop = TakePending(trigger);
				if (!interop)
					continue;
				if (Submits[submitIndex].pNext || Submits[submitIndex].flags)
				{
					spdlog::error("Unsupported Vulkan submit2 extension chain for frame generation.");
					interop->CancelPending(trigger);
					return Original(Queue, SubmitCount, Submits, Fence);
				}

				VulkanInteropSubmission submission = { .TriggerCommandBuffer = trigger };
				if (!interop->PrepareSubmission(Queue, &submission))
				{
					spdlog::error("Failed to prepare Vulkan-DX12 frame submission.");
					interop->CancelPending(trigger);
					return Original(Queue, SubmitCount, Submits, Fence);
				}

				const auto& target = Submits[submitIndex];
				std::vector<VkCommandBufferSubmitInfo> beforeCommands(
					target.pCommandBufferInfos,
					target.pCommandBufferInfos + commandIndex + 1);
				std::vector<VkCommandBufferSubmitInfo> afterCommands(
					target.pCommandBufferInfos + commandIndex + 1,
					target.pCommandBufferInfos + target.commandBufferInfoCount);

				const VkSemaphoreSubmitInfo copyInSignal = {
					.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = submission.InteropSemaphore,
					.value = submission.CopyInValue,
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
				VkSubmitInfo2 before = target;
				before.commandBufferInfoCount = static_cast<uint32_t>(beforeCommands.size());
				before.pCommandBufferInfos = beforeCommands.data();
				before.signalSemaphoreInfoCount = 1;
				before.pSignalSemaphoreInfos = &copyInSignal;

				const VkSemaphoreSubmitInfo dx12Wait = {
					.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = submission.InteropSemaphore,
					.value = submission.DX12DoneValue,
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
				const VkSemaphoreSubmitInfo copyBackSignal = {
					.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = submission.CopyBackSemaphore,
					.value = submission.CopyBackValue,
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
				const VkCommandBufferSubmitInfo copyBackCommand = {
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
					.commandBuffer = submission.CopyBackCommandBuffer,
				};
				const VkSubmitInfo2 copyBack = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
					.waitSemaphoreInfoCount = 1,
					.pWaitSemaphoreInfos = &dx12Wait,
					.commandBufferInfoCount = 1,
					.pCommandBufferInfos = &copyBackCommand,
					.signalSemaphoreInfoCount = 1,
					.pSignalSemaphoreInfos = &copyBackSignal,
				};

				const VkSemaphoreSubmitInfo finalWait = {
					.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = submission.CopyBackSemaphore,
					.value = submission.CopyBackValue,
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
				const VkSubmitInfo2 final = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
					.waitSemaphoreInfoCount = 1,
					.pWaitSemaphoreInfos = &finalWait,
					.commandBufferInfoCount = static_cast<uint32_t>(afterCommands.size()),
					.pCommandBufferInfos = afterCommands.data(),
					.signalSemaphoreInfoCount = target.signalSemaphoreInfoCount,
					.pSignalSemaphoreInfos = target.pSignalSemaphoreInfos,
				};

				std::vector<VkSubmitInfo2> injected;
				injected.reserve(SubmitCount + 2);
				for (uint32_t i = 0; i < SubmitCount; i++)
				{
					if (i == submitIndex)
					{
						injected.push_back(before);
						injected.push_back(copyBack);
						injected.push_back(final);
					}
					else
					{
						injected.push_back(Submits[i]);
					}
				}

				if (!interop->ExecuteSubmission(submission))
				{
					spdlog::error("Failed to queue DX12 frame-generation work.");
					interop->CancelSubmission(submission);
					return Original(Queue, SubmitCount, Submits, Fence);
				}

				const auto result = Original(Queue, static_cast<uint32_t>(injected.size()), injected.data(), Fence);
				if (result == VK_SUCCESS)
					interop->CommitSubmission(submission);
				else
				{
					interop->CancelSubmission(submission);
				}
				return result;
			}
		}

		return Original(Queue, SubmitCount, Submits, Fence);
	}

	VkResult VKAPI_CALL HookQueueSubmit(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo *Submits, VkFence Fence)
	{
		return InjectLegacy(g_QueueSubmit, Queue, SubmitCount, Submits, Fence);
	}

	VkResult VKAPI_CALL HookQueueSubmit2(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo2 *Submits, VkFence Fence)
	{
		return InjectSubmit2(g_QueueSubmit2, Queue, SubmitCount, Submits, Fence);
	}

	VkResult VKAPI_CALL HookQueueSubmit2KHR(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo2 *Submits, VkFence Fence)
	{
		return InjectSubmit2(g_QueueSubmit2KHR, Queue, SubmitCount, Submits, Fence);
	}
}

bool VulkanSubmitHooks::Initialize(VkDevice Device)
{
	std::scoped_lock lock(g_Mutex);
	if (g_Initialized)
		return true;

	g_QueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(vkGetDeviceProcAddr(Device, "vkQueueSubmit"));
	g_QueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(Device, "vkQueueSubmit2"));
	g_QueueSubmit2KHR = reinterpret_cast<PFN_vkQueueSubmit2KHR>(vkGetDeviceProcAddr(Device, "vkQueueSubmit2KHR"));
	if (!g_QueueSubmit)
		return false;

	if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
		DetourAttach(reinterpret_cast<PVOID *>(&g_QueueSubmit), HookQueueSubmit) != NO_ERROR)
	{
		DetourTransactionAbort();
		return false;
	}

	if (g_QueueSubmit2 && DetourAttach(reinterpret_cast<PVOID *>(&g_QueueSubmit2), HookQueueSubmit2) != NO_ERROR)
	{
		DetourTransactionAbort();
		return false;
	}
	if (g_QueueSubmit2KHR && reinterpret_cast<void *>(g_QueueSubmit2KHR) != reinterpret_cast<void *>(g_QueueSubmit2) &&
		DetourAttach(reinterpret_cast<PVOID *>(&g_QueueSubmit2KHR), HookQueueSubmit2KHR) != NO_ERROR)
	{
		DetourTransactionAbort();
		return false;
	}

	g_Initialized = DetourTransactionCommit() == NO_ERROR;
	return g_Initialized;
}

void VulkanSubmitHooks::RegisterPending(VkCommandBuffer CommandBuffer, std::shared_ptr<VulkanDX12Interop> Interop)
{
	std::scoped_lock lock(g_Mutex);
	g_Pending[CommandBuffer] = std::move(Interop);
}

void VulkanSubmitHooks::Unregister(VulkanDX12Interop *Interop)
{
	std::scoped_lock lock(g_Mutex);
	std::erase_if(
		g_Pending,
		[Interop](const auto& entry)
	{
		return entry.second.get() == Interop;
	});
}
