#include "ngx_abi.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace test_support
{
	namespace
	{
		template<typename T>
		T Resolve(HMODULE module, const char *name)
		{
			const FARPROC address = GetProcAddress(module, name);
			if (address == nullptr)
			{
				throw std::runtime_error(std::string("Missing NGX export: ") + name);
			}
			return reinterpret_cast<T>(address);
		}

		NgxFunctions ResolveFunctions(HMODULE module)
		{
			NgxFunctions functions {};
			functions.get_api_version = Resolve<NgxFunctions::GetVersion>(module, "NVSDK_NGX_GetAPIVersion");
			functions.get_application_id = Resolve<NgxFunctions::GetVersion>(module, "NVSDK_NGX_GetApplicationId");
			functions.get_driver_version = Resolve<NgxFunctions::GetVersion>(module, "NVSDK_NGX_GetDriverVersion");
			functions.get_driver_version_ex = Resolve<NgxFunctions::GetDriverVersionEx>(module, "NVSDK_NGX_GetDriverVersionEx");
			functions.get_gpu_architecture = Resolve<NgxFunctions::GetVersion>(module, "NVSDK_NGX_GetGPUArchitecture");
			functions.get_snippet_version = Resolve<NgxFunctions::GetVersion>(module, "NVSDK_NGX_GetSnippetVersion");
			functions.vulkan_create_feature1 = Resolve<NgxFunctions::CreateFeature1>(module, "NVSDK_NGX_VULKAN_CreateFeature1");
			functions.vulkan_create_feature = Resolve<NgxFunctions::CreateFeature>(module, "NVSDK_NGX_VULKAN_CreateFeature");
			functions.vulkan_evaluate_feature = Resolve<NgxFunctions::EvaluateFeature>(module, "NVSDK_NGX_VULKAN_EvaluateFeature");
			functions.vulkan_get_feature_requirements =
				Resolve<NgxFunctions::GetFeatureRequirements>(module, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
			functions.vulkan_get_scratch_buffer_size =
				Resolve<NgxFunctions::GetScratchBufferSize>(module, "NVSDK_NGX_VULKAN_GetScratchBufferSize");
			functions.vulkan_init_ext2 = Resolve<NgxFunctions::InitExt2>(module, "NVSDK_NGX_VULKAN_Init_Ext2");
			functions.vulkan_init_ext = Resolve<NgxFunctions::InitExt>(module, "NVSDK_NGX_VULKAN_Init_Ext");
			functions.vulkan_init = Resolve<NgxFunctions::Init>(module, "NVSDK_NGX_VULKAN_Init");
			functions.vulkan_populate_device_parameters =
				Resolve<NgxFunctions::PopulateDeviceParameters>(module, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl");
			functions.vulkan_populate_parameters =
				Resolve<NgxFunctions::PopulateParameters>(module, "NVSDK_NGX_VULKAN_PopulateParameters_Impl");
			functions.vulkan_release_feature = Resolve<NgxFunctions::ReleaseFeature>(module, "NVSDK_NGX_VULKAN_ReleaseFeature");
			functions.vulkan_shutdown = Resolve<NgxFunctions::Shutdown>(module, "NVSDK_NGX_VULKAN_Shutdown");
			functions.vulkan_shutdown1 = Resolve<NgxFunctions::Shutdown1>(module, "NVSDK_NGX_VULKAN_Shutdown1");
			return functions;
		}
	}

	NgxLibrary::NgxLibrary(const std::filesystem::path& path)
	{
		Load(path);
	}

	NgxLibrary::~NgxLibrary()
	{
		Unload();
	}

	NgxLibrary::NgxLibrary(NgxLibrary&& other) noexcept : module_(std::exchange(other.module_, nullptr)), functions_(other.functions_)
	{
		other.functions_ = {};
	}

	NgxLibrary& NgxLibrary::operator=(NgxLibrary&& other) noexcept
	{
		if (this != &other)
		{
			Unload();
			module_ = std::exchange(other.module_, nullptr);
			functions_ = other.functions_;
			other.functions_ = {};
		}
		return *this;
	}

	void NgxLibrary::Load(const std::filesystem::path& path)
	{
		Unload();
		HMODULE module = LoadLibraryW(path.c_str());
		if (module == nullptr)
		{
			throw std::runtime_error("LoadLibraryW failed for NGX library, error " + std::to_string(GetLastError()));
		}

		try
		{
			functions_ = ResolveFunctions(module);
			module_ = module;
		}
		catch (...)
		{
			FreeLibrary(module);
			throw;
		}
	}

	void NgxLibrary::Unload() noexcept
	{
		functions_ = {};
		if (module_ != nullptr)
		{
			FreeLibrary(module_);
			module_ = nullptr;
		}
	}

	bool NgxLibrary::IsLoaded() const noexcept
	{
		return module_ != nullptr;
	}

	HMODULE NgxLibrary::NativeHandle() const noexcept
	{
		return module_;
	}

	const NgxFunctions& NgxLibrary::Functions() const noexcept
	{
		return functions_;
	}
}
