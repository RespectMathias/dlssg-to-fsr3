#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR

#include "vulkan_ngx_e2e_backend.h"

#include <Windows.h>
#include <vulkan/vulkan.h>

#include "../../support/ngx_abi.h"
#include "../../support/ngx_parameters.h"
#include "../../support/procedural_scene.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>

namespace vulkan_ngx_e2e
{
	namespace
	{
		constexpr uint32_t kNgxSuccess = test_support::ngx_success;
		constexpr uint32_t kNgxInvalidParameter = test_support::ngx_invalid_parameter;
		constexpr uint8_t kOutputSentinel = 0x5a;

		struct NGXVulkanResourceHandle
		{
			struct
			{
				VkImageView view;
				VkImage image;
				VkImageSubresourceRange subresource;
				VkFormat format;
				uint32_t width;
				uint32_t height;
			} imageMetadata;
			uint32_t type;
		};


		struct Image
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImageAspectFlags aspect = 0;
			bool initialized = false;
			NGXVulkanResourceHandle ngx = {};
		};

		struct Buffer
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkDeviceSize size = 0;
			bool coherent = false;
		};

		VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
		{
			return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
		}

		bool HasName(std::span<const VkExtensionProperties> properties, const char* name)
		{
			return std::ranges::any_of(properties, [name](const VkExtensionProperties& property)
			{
				return std::strcmp(property.extensionName, name) == 0;
			});
		}

		bool HasLayer(std::span<const VkLayerProperties> properties, const char* name)
		{
			return std::ranges::any_of(properties, [name](const VkLayerProperties& property)
			{
				return std::strcmp(property.layerName, name) == 0;
			});
		}
	}

	struct Backend::Impl
	{
		using InitFn = test_support::NgxFunctions::Init;
		using PopulateFn = test_support::NgxFunctions::PopulateParameters;
		using CreateFn = test_support::NgxFunctions::CreateFeature;
		using EvaluateFn = test_support::NgxFunctions::EvaluateFeature;
		using ReleaseFn = test_support::NgxFunctions::ReleaseFeature;
		using ShutdownFn = test_support::NgxFunctions::Shutdown1;

		Config config;
		std::string error;
		std::vector<std::string> validationErrors;
		std::mutex validationMutex;
		bool validationEnabled = false;
		bool unsupported = false;
		bool capabilityQueryFailed = false;
		uint32_t instanceApiVersion = VK_API_VERSION_1_0;

		VkInstance instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t queueFamily = UINT32_MAX;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		std::array<VkCommandBuffer, 3> frameCommands = {};
		VkSemaphore completionTimeline = VK_NULL_HANDLE;
		uint64_t completionValue = 0;
		PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
		PFN_vkWaitSemaphores waitSemaphores = nullptr;

		Image color;
		Image depth;
		Image motion;
		Image outputReal;
		Image outputInterpolated;
		Buffer readback;
		VkDeviceSize interpolatedReadbackOffset = 0;

		HMODULE module = nullptr;
		InitFn init = nullptr;
		PopulateFn populate = nullptr;
		CreateFn create = nullptr;
		EvaluateFn evaluate = nullptr;
		ReleaseFn release = nullptr;
		ShutdownFn shutdownNgx = nullptr;
		test_support::NgxParameterBag parameters;
		test_support::NgxHandle* feature = nullptr;

		~Impl()
		{
			Shutdown();

			if (instance && debugMessenger)
			{
				const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
					vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
				if (destroy)
					destroy(instance, debugMessenger, nullptr);
			}
			if (instance)
				vkDestroyInstance(instance, nullptr);

			// DLL owns process-wide Detours hooks and does not detach them. Keep module resident.
			module = nullptr;
		}

		void Shutdown()
		{
			ReleaseFeature();
			if (device && shutdownNgx)
				shutdownNgx(device);
			shutdownNgx = nullptr;
			DestroyBuffer(readback);
			DestroyImage(outputInterpolated);
			DestroyImage(outputReal);
			DestroyImage(motion);
			DestroyImage(depth);
			DestroyImage(color);
			if (device && completionTimeline)
				vkDestroySemaphore(device, completionTimeline, nullptr);
			completionTimeline = VK_NULL_HANDLE;
			if (device && commandPool)
				vkDestroyCommandPool(device, commandPool, nullptr);
			commandPool = VK_NULL_HANDLE;
			frameCommands = {};
			if (device)
				vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}

		void ReleaseFeature()
		{
			if (feature && release)
				release(feature);
			feature = nullptr;
		}

		static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT type,
			const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
			void* userData)
		{
			if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) == 0 ||
				(type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0 || !callbackData || !callbackData->pMessage)
				return VK_FALSE;
			auto& self = *static_cast<Impl*>(userData);
			std::scoped_lock lock(self.validationMutex);
			self.validationErrors.emplace_back(callbackData->pMessage);
			return VK_FALSE;
		}

		bool Fail(std::string message)
		{
			error = std::move(message);
			return false;
		}

		bool FailUnsupported(std::string message)
		{
			unsupported = true;
			return Fail(std::move(message));
		}

		bool Initialize()
		{
			if (config.width <= 32 || config.height <= 32)
				return Fail("dimensions must exceed 32 pixels");
			if (GetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", nullptr, 0) == 0)
				SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~implicit~");
			if (!CreateInstance() || !SelectPhysicalDevice() || !CreateDevice() || !CreateResources() || !LoadDll())
				return false;

			parameters.Set5("Width", config.width);
			parameters.Set5("Height", config.height);
			parameters.SetVoidPointer("DLSSG.Backbuffer", &color.ngx);
			parameters.SetVoidPointer("DLSSG.Depth", &depth.ngx);
			parameters.SetVoidPointer("DLSSG.MVecs", &motion.ngx);
			parameters.SetVoidPointer("DLSSG.OutputReal", &outputReal.ngx);
			parameters.SetVoidPointer("DLSSG.OutputInterpolated", &outputInterpolated.ngx);
			parameters.SetVoidPointer("DLSSG.HUDLess", nullptr);
			parameters.Set5("DLSSG.DepthSubrectWidth", config.width);
			parameters.Set5("DLSSG.DepthSubrectHeight", config.height);
			parameters.Set5("DLSSG.MVecsSubrectWidth", config.width);
			parameters.Set5("DLSSG.MVecsSubrectHeight", config.height);
			parameters.Set5("DLSSG.MultiFrameCount", 1);
			parameters.Set5("DLSSG.MultiFrameIndex", 1);
			parameters.Set5("DLSSG.ColorBuffersHDR", 0);
			parameters.Set5("DLSSG.DepthInverted", 0);
			parameters.Set5("DLSSG.MvecDilated", 0);
			parameters.Set5("DLSSG.MvecJittered", 0);
			parameters.Set2("DLSSG.MvecScaleX", static_cast<float>(config.width));
			parameters.Set2("DLSSG.MvecScaleY", static_cast<float>(config.height));
			parameters.Set2("DLSSG.JitterOffsetX", 0.0f);
			parameters.Set2("DLSSG.JitterOffsetY", 0.0f);
			parameters.Set2("DLSSG.CameraNear", 0.1f);
			parameters.Set2("DLSSG.CameraFar", 1000.0f);
			parameters.Set2("DLSSG.CameraFOV", 1.0f);

			if (init(nullptr, nullptr, instance, physicalDevice, device, 0) != kNgxSuccess)
				return Fail("NVSDK_NGX_VULKAN_Init failed");
			if (populate(&parameters) != kNgxSuccess)
				return Fail("NVSDK_NGX_VULKAN_PopulateParameters_Impl failed");

			VkCommandBuffer createCommand = VK_NULL_HANDLE;
			if (!AllocateCommandBuffers(1, &createCommand))
				return false;
			const uint32_t result = create(createCommand, nullptr, &parameters, &feature);
			vkFreeCommandBuffers(device, commandPool, 1, &createCommand);
			if (result != kNgxSuccess || !feature)
				return Fail("NVSDK_NGX_VULKAN_CreateFeature failed");
			return true;
		}

		bool CreateInstance()
		{
			uint32_t loaderVersion = VK_API_VERSION_1_0;
			const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
				vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
			if (enumerateVersion)
				enumerateVersion(&loaderVersion);
			if (loaderVersion < VK_API_VERSION_1_2)
				return FailUnsupported("Vulkan 1.2 loader is required");
			instanceApiVersion = std::min(loaderVersion, VK_API_VERSION_1_3);

			uint32_t extensionCount = 0;
			vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> extensions(extensionCount);
			vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
			uint32_t layerCount = 0;
			vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> layers(layerCount);
			vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

			std::vector<const char*> enabledExtensions;
			std::vector<const char*> enabledLayers;
			validationEnabled = config.enableValidation && HasName(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
				HasLayer(layers, "VK_LAYER_KHRONOS_validation");
			if (config.enableValidation && !validationEnabled)
				return Fail("Vulkan validation layer and VK_EXT_debug_utils are required when validation is enabled");
			if (validationEnabled)
			{
				enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
				enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
			}

			const VkApplicationInfo applicationInfo = {
				.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				.pApplicationName = "vulkan_ngx_e2e",
				.applicationVersion = 1,
				.pEngineName = "vulkan_ngx_e2e",
				.engineVersion = 1,
				.apiVersion = instanceApiVersion,
			};
			VkDebugUtilsMessengerCreateInfoEXT debugInfo = {
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
				.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
				.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
					VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
				.pfnUserCallback = DebugCallback,
				.pUserData = this,
			};
			const VkInstanceCreateInfo createInfo = {
				.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
				.pNext = validationEnabled ? &debugInfo : nullptr,
				.pApplicationInfo = &applicationInfo,
				.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
				.ppEnabledLayerNames = enabledLayers.data(),
				.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
				.ppEnabledExtensionNames = enabledExtensions.data(),
			};
			if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
				return Fail("vkCreateInstance failed");

			if (validationEnabled)
			{
				const auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
					vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
				if (!createDebug || createDebug(instance, &debugInfo, nullptr, &debugMessenger) != VK_SUCCESS)
					return Fail("vkCreateDebugUtilsMessengerEXT failed");
			}
			return true;
		}

		bool SelectPhysicalDevice()
		{
			uint32_t count = 0;
			if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0)
				return FailUnsupported("no Vulkan physical devices found");
			std::vector<VkPhysicalDevice> devices(count);
			vkEnumeratePhysicalDevices(instance, &count, devices.data());

			for (const auto candidate : devices)
			{
				VkPhysicalDeviceIDProperties id = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
				VkPhysicalDeviceProperties2 properties = {
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
					.pNext = &id,
				};
				vkGetPhysicalDeviceProperties2(candidate, &properties);
				if (!id.deviceLUIDValid || properties.properties.apiVersion < VK_API_VERSION_1_2)
					continue;

				uint32_t familyCount = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
				std::vector<VkQueueFamilyProperties> families(familyCount);
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
				uint32_t selectedFamily = UINT32_MAX;
				for (uint32_t i = 0; i < familyCount; ++i)
				{
					if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
						(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
					{
						selectedFamily = i;
						break;
					}
				}
				if (selectedFamily == UINT32_MAX)
				{
					for (uint32_t i = 0; i < familyCount; ++i)
					{
						if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
						{
							selectedFamily = i;
							break;
						}
					}
				}
				if (selectedFamily == UINT32_MAX || !SupportsInterop(candidate))
				{
					if (capabilityQueryFailed)
						return false;
					continue;
				}

				physicalDevice = candidate;
				queueFamily = selectedFamily;
				return true;
			}
			return FailUnsupported("no LUID Vulkan adapter supports required DX12 interop features");
		}

		bool SupportsInterop(VkPhysicalDevice candidate)
		{
			uint32_t extensionCount = 0;
			if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr) != VK_SUCCESS)
			{
				capabilityQueryFailed = true;
				return Fail("vkEnumerateDeviceExtensionProperties count failed");
			}
			std::vector<VkExtensionProperties> extensions(extensionCount);
			if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS)
			{
				capabilityQueryFailed = true;
				return Fail("vkEnumerateDeviceExtensionProperties data failed");
			}
			const std::array required = {
				VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
				VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
			};
			if (!std::ranges::all_of(required, [&extensions](const char* name) { return HasName(extensions, name); }))
				return false;
			if (instanceApiVersion < VK_API_VERSION_1_3 && !HasName(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))
				return false;

			VkPhysicalDeviceSynchronization2Features synchronization2 = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
			};
			VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
				.pNext = &synchronization2,
			};
			VkPhysicalDeviceFeatures2 features = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
				.pNext = &timeline,
			};
			vkGetPhysicalDeviceFeatures2(candidate, &features);
			if (!features.features.shaderStorageImageWriteWithoutFormat || !timeline.timelineSemaphore || !synchronization2.synchronization2)
				return false;

			const VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
				.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
			};
			VkExternalSemaphoreProperties semaphoreProperties = {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
			};
			vkGetPhysicalDeviceExternalSemaphoreProperties(candidate, &semaphoreInfo, &semaphoreProperties);
			if ((semaphoreProperties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0)
				return false;

			for (const VkFormat format : { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT })
			{
				const VkPhysicalDeviceExternalImageFormatInfo external = {
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
					.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
				};
				const VkPhysicalDeviceImageFormatInfo2 formatInfo = {
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
					.pNext = &external,
					.format = format,
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
				if (vkGetPhysicalDeviceImageFormatProperties2(candidate, &formatInfo, &imageProperties) != VK_SUCCESS ||
					(externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0)
					return false;
			}
			return true;
		}

		bool CreateDevice()
		{
			VkPhysicalDeviceProperties properties = {};
			vkGetPhysicalDeviceProperties(physicalDevice, &properties);
			uint32_t extensionCount = 0;
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> available(extensionCount);
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, available.data());

			std::vector<const char*> extensions = {
				VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
				VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
			};
			if (instanceApiVersion < VK_API_VERSION_1_3)
				extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
			if (properties.apiVersion < VK_API_VERSION_1_2)
				extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
			if (HasName(available, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME))
				extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
			if (HasName(available, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME))
				extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);

			const float priority = 1.0f;
			const VkDeviceQueueCreateInfo queueInfo = {
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = queueFamily,
				.queueCount = 1,
				.pQueuePriorities = &priority,
			};
			VkPhysicalDeviceSynchronization2Features synchronization2 = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
				.synchronization2 = VK_TRUE,
			};
			VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
				.pNext = &synchronization2,
				.timelineSemaphore = VK_TRUE,
			};
			VkPhysicalDeviceFeatures2 features = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
				.pNext = &timeline,
			};
			features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
			const VkDeviceCreateInfo createInfo = {
				.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.pNext = &features,
				.queueCreateInfoCount = 1,
				.pQueueCreateInfos = &queueInfo,
				.enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
				.ppEnabledExtensionNames = extensions.data(),
			};
			if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS)
				return Fail("vkCreateDevice failed");
			vkGetDeviceQueue(device, queueFamily, 0, &queue);

			queueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(device, "vkQueueSubmit2"));
			if (!queueSubmit2)
				queueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR"));
			waitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(vkGetDeviceProcAddr(device, "vkWaitSemaphores"));
			if (!waitSemaphores)
				waitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR"));
			if (!waitSemaphores || (config.submitApi == SubmitApi::Submit2 && !queueSubmit2))
				return Fail("requested timeline or submit2 entry point is unavailable");

			const VkCommandPoolCreateInfo poolInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
				.queueFamilyIndex = queueFamily,
			};
			if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
				return Fail("vkCreateCommandPool failed");

			const VkSemaphoreTypeCreateInfo typeInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
				.initialValue = 0,
			};
			const VkSemaphoreCreateInfo semaphoreInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = &typeInfo,
			};
			if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &completionTimeline) != VK_SUCCESS)
				return Fail("vkCreateSemaphore for completion timeline failed");
			return true;
		}

		uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const
		{
			VkPhysicalDeviceMemoryProperties properties = {};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
			for (uint32_t pass = 0; pass < 2; ++pass)
			{
				for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
				{
					const auto flags = properties.memoryTypes[i].propertyFlags;
					if ((bits & (1u << i)) && (flags & required) == required && (pass != 0 || (flags & preferred) == preferred))
						return i;
				}
			}
			return UINT32_MAX;
		}

		bool CreateImage(Image& target, VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage)
		{
			target.format = format;
			target.aspect = aspect;
			const VkImageCreateInfo imageInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = format,
				.extent = { config.width, config.height, 1 },
				.mipLevels = 1,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = usage,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			if (vkCreateImage(device, &imageInfo, nullptr, &target.image) != VK_SUCCESS)
				return Fail("vkCreateImage failed");

			VkMemoryRequirements requirements = {};
			vkGetImageMemoryRequirements(device, target.image, &requirements);
			const uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (memoryType == UINT32_MAX)
				return Fail("no device-local image memory type");
			const VkMemoryAllocateInfo allocation = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = requirements.size,
				.memoryTypeIndex = memoryType,
			};
			if (vkAllocateMemory(device, &allocation, nullptr, &target.memory) != VK_SUCCESS ||
				vkBindImageMemory(device, target.image, target.memory, 0) != VK_SUCCESS)
				return Fail("image memory allocation failed");

			const VkImageViewCreateInfo viewInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = target.image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = format,
				.subresourceRange = { aspect, 0, 1, 0, 1 },
			};
			if (vkCreateImageView(device, &viewInfo, nullptr, &target.view) != VK_SUCCESS)
				return Fail("vkCreateImageView failed");

			target.ngx.imageMetadata = {
				.view = target.view,
				.image = target.image,
				.subresource = { aspect, 0, 1, 0, 1 },
				.format = format,
				.width = config.width,
				.height = config.height,
			};
			target.ngx.type = 0;
			return true;
		}

		bool CreateBuffer(Buffer& target, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required)
		{
			target.size = size;
			const VkBufferCreateInfo bufferInfo = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = size,
				.usage = usage,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			if (vkCreateBuffer(device, &bufferInfo, nullptr, &target.buffer) != VK_SUCCESS)
				return Fail("vkCreateBuffer failed");
			VkMemoryRequirements requirements = {};
			vkGetBufferMemoryRequirements(device, target.buffer, &requirements);
			const uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits, required, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (memoryType == UINT32_MAX)
				return Fail("no host-visible buffer memory type");
			VkPhysicalDeviceMemoryProperties properties = {};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
			target.coherent = (properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
			const VkMemoryAllocateInfo allocation = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = requirements.size,
				.memoryTypeIndex = memoryType,
			};
			if (vkAllocateMemory(device, &allocation, nullptr, &target.memory) != VK_SUCCESS ||
				vkBindBufferMemory(device, target.buffer, target.memory, 0) != VK_SUCCESS)
				return Fail("buffer memory allocation failed");
			return true;
		}

		bool CreateResources()
		{
			const auto inputUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			const auto outputUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (!CreateImage(color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, inputUsage) ||
				!CreateImage(depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, inputUsage) ||
				!CreateImage(motion, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, inputUsage) ||
				!CreateImage(outputReal, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, outputUsage) ||
				!CreateImage(outputInterpolated, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, outputUsage))
				return false;

			VkPhysicalDeviceProperties properties = {};
			vkGetPhysicalDeviceProperties(physicalDevice, &properties);
			const VkDeviceSize imageBytes = static_cast<VkDeviceSize>(config.width) * config.height * 4;
			interpolatedReadbackOffset = AlignUp(imageBytes, properties.limits.optimalBufferCopyOffsetAlignment);
			return CreateBuffer(
				readback,
				interpolatedReadbackOffset + imageBytes,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		}

		template<typename T>
		bool LoadExport(T& function, const char* name)
		{
			function = reinterpret_cast<T>(GetProcAddress(module, name));
			return function != nullptr;
		}

		bool LoadDll()
		{
			SetEnvironmentVariableW(
				L"DLSSGTOFSR3_VulkanUseCopyForInputs",
				config.copyPath == CopyPath::Transfer ? L"1" : L"0");
			SetEnvironmentVariableW(
				L"DLSSGTOFSR3_VulkanUseCopyForOutput",
				config.copyPath == CopyPath::Transfer ? L"1" : L"0");

			std::wstring path = config.dll;
			if (path.empty())
			{
				std::array<wchar_t, 32768> environment = {};
				const DWORD length = GetEnvironmentVariableW(
					L"DLSSG_TO_FSR_DLL",
					environment.data(),
					static_cast<DWORD>(environment.size()));
				path = length != 0 && length < environment.size() ? environment.data() : L"dlssg_to_fsr.dll";
			}
			module = LoadLibraryW(path.c_str());
			if (!module)
				return Fail("DLL not found; set DLSSG_TO_FSR_DLL");

			if (!LoadExport(init, "NVSDK_NGX_VULKAN_Init") ||
				!LoadExport(populate, "NVSDK_NGX_VULKAN_PopulateParameters_Impl") ||
				!LoadExport(create, "NVSDK_NGX_VULKAN_CreateFeature") ||
				!LoadExport(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature") ||
				!LoadExport(release, "NVSDK_NGX_VULKAN_ReleaseFeature"))
				return Fail("DLL is missing Vulkan NGX exports");
			LoadExport(shutdownNgx, "NVSDK_NGX_VULKAN_Shutdown1");
			return true;
		}

		bool AllocateCommandBuffers(uint32_t count, VkCommandBuffer* commands)
		{
			const VkCommandBufferAllocateInfo allocation = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = commandPool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = count,
			};
			if (vkAllocateCommandBuffers(device, &allocation, commands) != VK_SUCCESS)
				return Fail("vkAllocateCommandBuffers failed");
			return true;
		}

		bool UploadBufferData(Buffer& upload, const FrameInput& input, std::array<VkDeviceSize, 5>& offsets)
		{
			const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(config.width) * config.height;
			const VkDeviceSize imageBytes = pixelCount * 4;
			for (size_t i = 0; i < offsets.size(); ++i)
				offsets[i] = imageBytes * i;
			if (!CreateBuffer(upload, imageBytes * offsets.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
				return false;

			void* mapped = nullptr;
			if (vkMapMemory(device, upload.memory, 0, upload.size, 0, &mapped) != VK_SUCCESS)
				return Fail("vkMapMemory for upload failed");
			auto* bytes = static_cast<uint8_t*>(mapped);
			std::memcpy(bytes + offsets[0], input.colorRgba8.data(), static_cast<size_t>(imageBytes));
			std::memcpy(bytes + offsets[1], input.depth.data(), static_cast<size_t>(imageBytes));
			auto* halfMotion = reinterpret_cast<uint16_t*>(bytes + offsets[2]);
			for (size_t i = 0; i < input.motion.size(); ++i)
			{
				halfMotion[i * 2] = test_support::FloatToHalf(input.motion[i][0]);
				halfMotion[i * 2 + 1] = test_support::FloatToHalf(input.motion[i][1]);
			}
			std::memset(bytes + offsets[3], kOutputSentinel, static_cast<size_t>(imageBytes));
			std::memset(bytes + offsets[4], kOutputSentinel, static_cast<size_t>(imageBytes));
			if (!upload.coherent)
			{
				const VkMappedMemoryRange range = {
					.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
					.memory = upload.memory,
					.offset = 0,
					.size = VK_WHOLE_SIZE,
				};
				if (vkFlushMappedMemoryRanges(device, 1, &range) != VK_SUCCESS)
				{
					vkUnmapMemory(device, upload.memory);
					return Fail("vkFlushMappedMemoryRanges failed");
				}
			}
			vkUnmapMemory(device, upload.memory);
			return true;
		}

		void RecordImageUpload(VkCommandBuffer command, const Buffer& upload, VkDeviceSize offset, Image& target)
		{
			const VkImageSubresourceRange range = { target.aspect, 0, 1, 0, 1 };
			VkImageMemoryBarrier barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = target.initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VkAccessFlags {},
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.oldLayout = target.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = target.image,
				.subresourceRange = range,
			};
			vkCmdPipelineBarrier(
				command,
				target.initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				1,
				&barrier);
			const VkBufferImageCopy copy = {
				.bufferOffset = offset,
				.imageSubresource = { target.aspect, 0, 0, 1 },
				.imageExtent = { config.width, config.height, 1 },
			};
			vkCmdCopyBufferToImage(command, upload.buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			vkCmdPipelineBarrier(
				command,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				1,
				&barrier);
			target.initialized = true;
		}

		bool RecordUpload(VkCommandBuffer command, const Buffer& upload, const std::array<VkDeviceSize, 5>& offsets)
		{
			const VkCommandBufferBeginInfo begin = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
				return Fail("vkBeginCommandBuffer for upload failed");
			RecordImageUpload(command, upload, offsets[0], color);
			RecordImageUpload(command, upload, offsets[1], depth);
			RecordImageUpload(command, upload, offsets[2], motion);
			RecordImageUpload(command, upload, offsets[3], outputReal);
			RecordImageUpload(command, upload, offsets[4], outputInterpolated);
			if (vkEndCommandBuffer(command) != VK_SUCCESS)
				return Fail("vkEndCommandBuffer for upload failed");
			return true;
		}

		bool RecordEvaluation(VkCommandBuffer command, uint32_t* ngxResult)
		{
			const VkCommandBufferBeginInfo begin = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
				return Fail("vkBeginCommandBuffer for evaluation failed");
			*ngxResult = evaluate(command, feature, &parameters);
			if (*ngxResult != kNgxSuccess && *ngxResult != kNgxInvalidParameter)
				return Fail("NVSDK_NGX_VULKAN_EvaluateFeature returned an unexpected result");
			if (vkEndCommandBuffer(command) != VK_SUCCESS)
				return Fail("vkEndCommandBuffer for evaluation failed");
			return true;
		}

		void RecordImageReadback(VkCommandBuffer command, Image& source, VkDeviceSize offset)
		{
			VkImageMemoryBarrier barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = source.image,
				.subresourceRange = { source.aspect, 0, 1, 0, 1 },
			};
			vkCmdPipelineBarrier(
				command,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				1,
				&barrier);
			const VkBufferImageCopy copy = {
				.bufferOffset = offset,
				.imageSubresource = { source.aspect, 0, 0, 1 },
				.imageExtent = { config.width, config.height, 1 },
			};
			vkCmdCopyImageToBuffer(command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			vkCmdPipelineBarrier(
				command,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0,
				0,
				nullptr,
				0,
				nullptr,
				1,
				&barrier);
		}

		bool RecordReadback(VkCommandBuffer command)
		{
			const VkCommandBufferBeginInfo begin = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
				return Fail("vkBeginCommandBuffer for readback failed");
			RecordImageReadback(command, outputReal, 0);
			RecordImageReadback(command, outputInterpolated, interpolatedReadbackOffset);
			if (vkEndCommandBuffer(command) != VK_SUCCESS)
				return Fail("vkEndCommandBuffer for readback failed");
			return true;
		}

		bool SubmitAndWait(std::span<const VkCommandBuffer> commands)
		{
			VkFence fence = VK_NULL_HANDLE;
			const VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
				return Fail("vkCreateFence failed");
			++completionValue;

			VkResult submitResult = VK_ERROR_UNKNOWN;
			if (config.submitApi == SubmitApi::Legacy)
			{
				const VkTimelineSemaphoreSubmitInfo timelineInfo = {
					.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
					.signalSemaphoreValueCount = 1,
					.pSignalSemaphoreValues = &completionValue,
				};
				const VkSubmitInfo submit = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					.pNext = &timelineInfo,
					.commandBufferCount = static_cast<uint32_t>(commands.size()),
					.pCommandBuffers = commands.data(),
					.signalSemaphoreCount = 1,
					.pSignalSemaphores = &completionTimeline,
				};
				submitResult = vkQueueSubmit(queue, 1, &submit, fence);
			}
			else
			{
				std::vector<VkCommandBufferSubmitInfo> commandInfos;
				commandInfos.reserve(commands.size());
				for (const auto command : commands)
				{
					commandInfos.push_back({
						.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
						.commandBuffer = command,
					});
				}
				const VkSemaphoreSubmitInfo signal = {
					.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = completionTimeline,
					.value = completionValue,
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
				const VkSubmitInfo2 submit = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
					.commandBufferInfoCount = static_cast<uint32_t>(commandInfos.size()),
					.pCommandBufferInfos = commandInfos.data(),
					.signalSemaphoreInfoCount = 1,
					.pSignalSemaphoreInfos = &signal,
				};
				submitResult = queueSubmit2(queue, 1, &submit, fence);
			}

			if (submitResult != VK_SUCCESS)
			{
				vkDestroyFence(device, fence, nullptr);
				return Fail("queue submission failed");
			}
			const VkResult fenceResult = vkWaitForFences(device, 1, &fence, VK_TRUE, config.waitTimeoutNanoseconds);
			if (fenceResult == VK_TIMEOUT)
				vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
			vkDestroyFence(device, fence, nullptr);
			if (fenceResult != VK_SUCCESS)
				return Fail(fenceResult == VK_TIMEOUT ? "finite queue fence wait timed out" : "vkWaitForFences failed");

			const VkSemaphoreWaitInfo waitInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores = &completionTimeline,
				.pValues = &completionValue,
			};
			const VkResult timelineResult = waitSemaphores(device, &waitInfo, config.waitTimeoutNanoseconds);
			if (timelineResult == VK_TIMEOUT)
				waitSemaphores(device, &waitInfo, UINT64_MAX);
			if (timelineResult != VK_SUCCESS)
				return Fail(timelineResult == VK_TIMEOUT ? "finite timeline wait timed out" : "vkWaitSemaphores failed");
			return true;
		}

		bool ReadOutput(FrameOutput& output)
		{
			void* mapped = nullptr;
			if (vkMapMemory(device, readback.memory, 0, readback.size, 0, &mapped) != VK_SUCCESS)
				return Fail("vkMapMemory for readback failed");
			if (!readback.coherent)
			{
				const VkMappedMemoryRange range = {
					.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
					.memory = readback.memory,
					.offset = 0,
					.size = VK_WHOLE_SIZE,
				};
				if (vkInvalidateMappedMemoryRanges(device, 1, &range) != VK_SUCCESS)
				{
					vkUnmapMemory(device, readback.memory);
					return Fail("vkInvalidateMappedMemoryRanges failed");
				}
			}
			const size_t bytes = static_cast<size_t>(config.width) * config.height * 4;
			const auto* source = static_cast<const uint8_t*>(mapped);
			output.outputRealRgba8.assign(source, source + bytes);
			output.outputInterpolatedRgba8.assign(
				source + interpolatedReadbackOffset,
				source + interpolatedReadbackOffset + bytes);
			vkUnmapMemory(device, readback.memory);
			return true;
		}

		bool RunFrame(const FrameInput& input, FrameOutput& output)
		{
			const size_t pixelCount = static_cast<size_t>(config.width) * config.height;
			if (input.colorRgba8.size() != pixelCount * 4 || input.depth.size() != pixelCount || input.motion.size() != pixelCount)
				return Fail("frame input dimensions do not match backend dimensions");

			parameters.Set5("DLSSG.EnableInterp", input.enableInterpolation ? 1u : 0u);
			parameters.Set5("DLSSG.Reset", input.reset ? 1u : 0u);
			if (vkResetCommandPool(device, commandPool, 0) != VK_SUCCESS)
				return Fail("vkResetCommandPool failed");

			Buffer upload;
			std::array<VkDeviceSize, 5> offsets = {};
			if (!UploadBufferData(upload, input, offsets))
			{
				DestroyBuffer(upload);
				return false;
			}

			if ((!frameCommands[0] && !AllocateCommandBuffers(static_cast<uint32_t>(frameCommands.size()), frameCommands.data())) ||
				!RecordUpload(frameCommands[0], upload, offsets) || !RecordEvaluation(frameCommands[1], &output.ngxResult) ||
				!RecordReadback(frameCommands[2]) || !SubmitAndWait(frameCommands) || !ReadOutput(output))
			{
				DestroyBuffer(upload);
				return false;
			}
			DestroyBuffer(upload);
			return true;
		}

		void DestroyImage(Image& image)
		{
			if (!device)
				return;
			if (image.view)
				vkDestroyImageView(device, image.view, nullptr);
			if (image.image)
				vkDestroyImage(device, image.image, nullptr);
			if (image.memory)
				vkFreeMemory(device, image.memory, nullptr);
			image = {};
		}

		void DestroyBuffer(Buffer& buffer)
		{
			if (!device)
				return;
			if (buffer.buffer)
				vkDestroyBuffer(device, buffer.buffer, nullptr);
			if (buffer.memory)
				vkFreeMemory(device, buffer.memory, nullptr);
			buffer = {};
		}
	};

	Backend::Backend() = default;

	Backend::~Backend()
	{
		delete impl_;
	}

	bool Backend::Initialize(const Config& config)
	{
		delete impl_;
		impl_ = new Impl;
		impl_->config = config;
		return impl_->Initialize();
	}

	bool Backend::RunFrame(const FrameInput& input, FrameOutput* output)
	{
		if (!impl_ || !output)
			return false;
		return impl_->RunFrame(input, *output);
	}

	void Backend::ReleaseFeature()
	{
		if (impl_)
			impl_->ReleaseFeature();
	}

	void Backend::Shutdown()
	{
		if (impl_)
			impl_->Shutdown();
	}

	const std::string& Backend::LastError() const
	{
		static const std::string notInitialized = "backend is not initialized";
		return impl_ ? impl_->error : notInitialized;
	}

	const std::vector<std::string>& Backend::ValidationErrors() const
	{
		static const std::vector<std::string> none;
		return impl_ ? impl_->validationErrors : none;
	}

	bool Backend::ValidationEnabled() const
	{
		return impl_ && impl_->validationEnabled;
	}

	bool Backend::Unsupported() const
	{
		return impl_ && impl_->unsupported;
	}
}
