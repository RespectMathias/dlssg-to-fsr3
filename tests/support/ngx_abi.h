#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>

struct ID3D12Resource;

namespace test_support
{
	using NgxResult = std::uint32_t;

	inline constexpr NgxResult ngx_success = 0x00000001U;
	inline constexpr NgxResult ngx_feature_not_found = 0xBAD00004U;
	inline constexpr NgxResult ngx_invalid_parameter = 0xBAD00005U;

	struct NgxHandle
	{
		std::uint32_t internal_id;
		std::uint32_t internal_feature_id;
	};

	struct NgxFeatureRequirementInfo
	{
		std::uint32_t flags;
		std::uint32_t required_gpu_architecture;
		char required_operating_system_version[32];
	};

	struct NgxInstanceParameters
	{
		virtual void SetVoidPointer(const char *name, void *value) = 0;
		virtual void Set2(const char *name, float value) = 0;
		virtual void Set3(const char *name, void *value) = 0;
		virtual void Set4(const char *name, std::uint32_t value) = 0;
		virtual void Set5(const char *name, std::uint32_t value) = 0;
		virtual void Set6(const char *name, void *value) = 0;
		virtual void Set7(const char *name, ID3D12Resource *value) = 0;
		virtual void Set8(const char *name, void *value) = 0;
		virtual NgxResult GetVoidPointer(const char *name, void **value) = 0;
		virtual NgxResult Get2(const char *name, float *value) = 0;
		virtual NgxResult Get3(const char *name, void *value) = 0;
		virtual NgxResult Get4(const char *name, std::uint32_t *value) = 0;
		virtual NgxResult Get5(const char *name, std::uint32_t *value) = 0;
		virtual NgxResult Get6(const char *name, void *value) = 0;
		virtual NgxResult Get7(const char *name, float *value) = 0;
		virtual NgxResult Get8(const char *name, void *value) = 0;
		virtual void Unknown() = 0;
	};

	struct NgxFunctions
	{
		using GetVersion = std::uint32_t (*)();
		using GetDriverVersionEx = NgxResult (*)(std::uint32_t *, std::uint32_t, std::uint32_t *);
		using CreateFeature1 = NgxResult (*)(VkDevice, VkCommandBuffer, void *, NgxInstanceParameters *, NgxHandle **);
		using CreateFeature = NgxResult (*)(VkCommandBuffer, void *, NgxInstanceParameters *, NgxHandle **);
		using EvaluateFeature = NgxResult (*)(VkCommandBuffer, NgxHandle *, NgxInstanceParameters *);
		using GetFeatureRequirements = NgxResult (*)(VkInstance, VkPhysicalDevice, void *, NgxFeatureRequirementInfo *);
		using GetScratchBufferSize = NgxResult (*)(void *, void *, std::uint64_t *);
		using InitExt2 = NgxResult (*)(
			void *, void *, VkInstance, VkPhysicalDevice, VkDevice, void *, std::uint32_t, NgxInstanceParameters *);
		using InitExt = NgxResult (*)(void *, void *, VkInstance, VkPhysicalDevice, VkDevice, std::uint32_t, void *);
		using Init = NgxResult (*)(void *, void *, VkInstance, VkPhysicalDevice, VkDevice, std::uint32_t);
		using PopulateDeviceParameters = NgxResult (*)(VkInstance, VkPhysicalDevice, VkDevice, void *, NgxInstanceParameters *);
		using PopulateParameters = NgxResult (*)(NgxInstanceParameters *);
		using ReleaseFeature = NgxResult (*)(NgxHandle *);
		using Shutdown = NgxResult (*)();
		using Shutdown1 = NgxResult (*)(VkDevice);

		GetVersion get_api_version = nullptr;
		GetVersion get_application_id = nullptr;
		GetVersion get_driver_version = nullptr;
		GetDriverVersionEx get_driver_version_ex = nullptr;
		GetVersion get_gpu_architecture = nullptr;
		GetVersion get_snippet_version = nullptr;
		CreateFeature1 vulkan_create_feature1 = nullptr;
		CreateFeature vulkan_create_feature = nullptr;
		EvaluateFeature vulkan_evaluate_feature = nullptr;
		GetFeatureRequirements vulkan_get_feature_requirements = nullptr;
		GetScratchBufferSize vulkan_get_scratch_buffer_size = nullptr;
		InitExt2 vulkan_init_ext2 = nullptr;
		InitExt vulkan_init_ext = nullptr;
		Init vulkan_init = nullptr;
		PopulateDeviceParameters vulkan_populate_device_parameters = nullptr;
		PopulateParameters vulkan_populate_parameters = nullptr;
		ReleaseFeature vulkan_release_feature = nullptr;
		Shutdown vulkan_shutdown = nullptr;
		Shutdown1 vulkan_shutdown1 = nullptr;
	};

	class NgxLibrary
	{
	public:
		NgxLibrary() = default;
		explicit NgxLibrary(const std::filesystem::path& path);
		~NgxLibrary();

		NgxLibrary(const NgxLibrary&) = delete;
		NgxLibrary& operator=(const NgxLibrary&) = delete;
		NgxLibrary(NgxLibrary&& other) noexcept;
		NgxLibrary& operator=(NgxLibrary&& other) noexcept;

		void Load(const std::filesystem::path& path);
		void Unload() noexcept;
		[[nodiscard]] bool IsLoaded() const noexcept;
		[[nodiscard]] HMODULE NativeHandle() const noexcept;
		[[nodiscard]] const NgxFunctions& Functions() const noexcept;

	private:
		HMODULE module_ = nullptr;
		NgxFunctions functions_ {};
	};
}
