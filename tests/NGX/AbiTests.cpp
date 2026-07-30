#include <gtest/gtest.h>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace
{
using NgxResult = std::uint32_t;

constexpr NgxResult NgxSuccess = 0x00000001;
constexpr NgxResult NgxUnsupported = 0xBAD00001;
constexpr NgxResult NgxInvalidParameter = 0xBAD00005;
constexpr std::uint32_t NgxGpuArchitecture = 0x000000C0;

struct NgxHandle
{
    std::uint32_t internalId;
    std::uint32_t internalFeatureId;
};

struct NgxFeatureRequirementInfo
{
    std::uint32_t flags;
    std::uint32_t requiredGpuArchitecture;
    char requiredOperatingSystemVersion[32];
};

static_assert(sizeof(NgxHandle) == 8);
static_assert(sizeof(NgxFeatureRequirementInfo) == 40);
static_assert(offsetof(NgxFeatureRequirementInfo, requiredOperatingSystemVersion) == 8);

class NgxModule
{
public:
    NgxModule()
        : handle_(LoadLibraryW(DLSSG_TEST_DLL_PATH))
    {
    }

    ~NgxModule()
    {
        if (handle_)
            FreeLibrary(handle_);
    }

    HMODULE handle() const
    {
        return handle_;
    }

    template<typename Function>
    Function get(const char *name) const
    {
        return reinterpret_cast<Function>(GetProcAddress(handle_, name));
    }

private:
    HMODULE handle_;
};

NgxModule &module()
{
    static NgxModule instance;
    return instance;
}

template<typename Function>
Function requireExport(const char *name)
{
    auto function = module().get<Function>(name);
    EXPECT_NE(function, nullptr) << name;
    return function;
}

TEST(NgxAbi, LoadsDll)
{
    ASSERT_NE(module().handle(), nullptr) << "LoadLibraryW failed with " << GetLastError();
}

TEST(NgxAbi, LoadsOptiScalerCompatibilityName)
{
	const HMODULE optiscaler = LoadLibraryW(DLSSG_TEST_OPTISCALER_DLL_PATH);
	ASSERT_NE(optiscaler, nullptr) << "LoadLibraryW failed with " << GetLastError();
	using GetVersion = std::uint32_t (*)();
	const auto getVersion = reinterpret_cast<GetVersion>(GetProcAddress(optiscaler, "NVSDK_NGX_GetAPIVersion"));
	ASSERT_NE(getVersion, nullptr);
	EXPECT_EQ(getVersion(), 19U);
	FreeLibrary(optiscaler);
}

TEST(FidelityFxApiAbi, StockDllExportsRequiredSurface)
{
	const auto apiPath = std::filesystem::path(DLSSG_TEST_DLL_PATH).parent_path() / L"amd_fidelityfx_dx12.dll";
	const HMODULE api = LoadLibraryExW(
		apiPath.c_str(),
		nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	ASSERT_NE(api, nullptr) << "LoadLibraryExW failed with " << GetLastError();

	constexpr const char *exports[] = {
		"ffxCreateContext",
		"ffxDestroyContext",
		"ffxConfigure",
		"ffxQuery",
		"ffxDispatch",
	};
	for (const auto name : exports)
		EXPECT_NE(GetProcAddress(api, name), nullptr) << name;

	FreeLibrary(api);
}

TEST(NgxAbi, ExportsProxySurface)
{
	ASSERT_NE(module().handle(), nullptr);

	constexpr const char *exports[] = {
		"MiniDumpWriteDump",
		"WinHttpOpen",
		"GetFileVersionInfoW",
	};
	for (const auto name : exports)
		EXPECT_NE(GetProcAddress(module().handle(), name), nullptr) << name;
}

TEST(NgxAbi, ExportsCompleteSurface)
{
    ASSERT_NE(module().handle(), nullptr);

    constexpr const char *exports[] = {
        "NVSDK_NGX_CUDA_CreateFeature",
        "NVSDK_NGX_CUDA_EvaluateFeature",
        "NVSDK_NGX_CUDA_GetScratchBufferSize",
        "NVSDK_NGX_CUDA_Init",
        "NVSDK_NGX_CUDA_ReleaseFeature",
        "NVSDK_NGX_CUDA_Shutdown",
        "NVSDK_NGX_GetAPIVersion",
        "NVSDK_NGX_GetApplicationId",
        "NVSDK_NGX_GetDriverVersion",
        "NVSDK_NGX_GetDriverVersionEx",
        "NVSDK_NGX_GetGPUArchitecture",
        "NVSDK_NGX_GetSnippetVersion",
        "NVSDK_NGX_ProcessCommand",
        "NVSDK_NGX_SetInfoCallback",
        "NVSDK_NGX_SetTelemetryEvaluateCallback",
        "NVSDK_NGX_D3D11_CreateFeature",
        "NVSDK_NGX_D3D11_EvaluateFeature",
        "NVSDK_NGX_D3D11_GetFeatureRequirements",
        "NVSDK_NGX_D3D11_GetScratchBufferSize",
        "NVSDK_NGX_D3D11_Init",
        "NVSDK_NGX_D3D11_Init_Ext",
        "NVSDK_NGX_D3D11_PopulateDeviceParameters_Impl",
        "NVSDK_NGX_D3D11_PopulateParameters_Impl",
        "NVSDK_NGX_D3D11_ReleaseFeature",
        "NVSDK_NGX_D3D11_Shutdown",
        "NVSDK_NGX_D3D11_Shutdown1",
        "NVSDK_NGX_D3D12_CreateFeature",
        "NVSDK_NGX_D3D12_EvaluateFeature",
        "NVSDK_NGX_D3D12_GetFeatureRequirements",
        "NVSDK_NGX_D3D12_GetScratchBufferSize",
        "NVSDK_NGX_D3D12_Init",
        "NVSDK_NGX_D3D12_Init_Ext",
        "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl",
        "NVSDK_NGX_D3D12_PopulateParameters_Impl",
        "NVSDK_NGX_D3D12_ReleaseFeature",
        "NVSDK_NGX_D3D12_Shutdown",
        "NVSDK_NGX_D3D12_Shutdown1",
        "NVSDK_NGX_VULKAN_CreateFeature",
        "NVSDK_NGX_VULKAN_CreateFeature1",
        "NVSDK_NGX_VULKAN_EvaluateFeature",
        "NVSDK_NGX_VULKAN_GetFeatureRequirements",
        "NVSDK_NGX_VULKAN_GetScratchBufferSize",
        "NVSDK_NGX_VULKAN_Init",
        "NVSDK_NGX_VULKAN_Init_Ext",
        "NVSDK_NGX_VULKAN_Init_Ext2",
        "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl",
        "NVSDK_NGX_VULKAN_PopulateParameters_Impl",
        "NVSDK_NGX_VULKAN_ReleaseFeature",
        "NVSDK_NGX_VULKAN_Shutdown",
        "NVSDK_NGX_VULKAN_Shutdown1",
        "NvOptimusEnablementCuda",
        "RefreshGlobalConfiguration",
    };

	static_assert(std::size(exports) == 52);

    for (const auto name : exports)
        EXPECT_NE(GetProcAddress(module().handle(), name), nullptr) << name;
}

TEST(NgxAbi, CommonConstantsAndDriverVersion)
{
    ASSERT_NE(module().handle(), nullptr);

    using ConstantFunction = std::uint32_t (*)();
    const auto getApiVersion = requireExport<ConstantFunction>("NVSDK_NGX_GetAPIVersion");
    const auto getApplicationId = requireExport<ConstantFunction>("NVSDK_NGX_GetApplicationId");
    const auto getDriverVersion = requireExport<ConstantFunction>("NVSDK_NGX_GetDriverVersion");
    const auto getGpuArchitecture = requireExport<ConstantFunction>("NVSDK_NGX_GetGPUArchitecture");
    const auto getSnippetVersion = requireExport<ConstantFunction>("NVSDK_NGX_GetSnippetVersion");
    using GetDriverVersionExFunction = NgxResult (*)(std::uint32_t *, std::uint32_t, std::uint32_t *);
    const auto getDriverVersionEx = requireExport<GetDriverVersionExFunction>("NVSDK_NGX_GetDriverVersionEx");
    ASSERT_NE(getApiVersion, nullptr);
    ASSERT_NE(getApplicationId, nullptr);
    ASSERT_NE(getDriverVersion, nullptr);
    ASSERT_NE(getGpuArchitecture, nullptr);
    ASSERT_NE(getSnippetVersion, nullptr);
    ASSERT_NE(getDriverVersionEx, nullptr);

    EXPECT_EQ(getApiVersion(), 19u);
    EXPECT_EQ(getApplicationId(), 0x0E658703u);
    EXPECT_EQ(getDriverVersion(), 0x02080000u);
    EXPECT_EQ(getGpuArchitecture(), NgxGpuArchitecture);
    EXPECT_EQ(getSnippetVersion(), 0x00030500u);
    EXPECT_EQ(getDriverVersionEx(nullptr, 0, nullptr), NgxInvalidParameter);

    std::array<std::uint32_t, 2> versions = {};
    std::uint32_t count = 0;
    EXPECT_EQ(getDriverVersionEx(versions.data(), static_cast<std::uint32_t>(versions.size()), &count), NgxSuccess);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(versions[0], 0x208u);
    EXPECT_EQ(versions[1], 0u);
}

TEST(NgxAbi, D3D11LifecycleIsUnsupportedWithNullInputs)
{
    ASSERT_NE(module().handle(), nullptr);

    using CreateFunction = NgxResult (*)(void *, void *, void *, NgxHandle **);
    using EvaluateFunction = NgxResult (*)(void *, NgxHandle *, void *);
    using InitFunction = NgxResult (*)(void *, const wchar_t *, void *, std::uint32_t);
    using ReleaseFunction = NgxResult (*)(NgxHandle *);
    using ShutdownFunction = NgxResult (*)();
    using ShutdownDeviceFunction = NgxResult (*)(void *);

    const auto create = requireExport<CreateFunction>("NVSDK_NGX_D3D11_CreateFeature");
    const auto evaluate = requireExport<EvaluateFunction>("NVSDK_NGX_D3D11_EvaluateFeature");
    const auto init = requireExport<InitFunction>("NVSDK_NGX_D3D11_Init");
    const auto initExt = requireExport<InitFunction>("NVSDK_NGX_D3D11_Init_Ext");
    const auto release = requireExport<ReleaseFunction>("NVSDK_NGX_D3D11_ReleaseFeature");
    const auto shutdown = requireExport<ShutdownFunction>("NVSDK_NGX_D3D11_Shutdown");
    const auto shutdownDevice = requireExport<ShutdownDeviceFunction>("NVSDK_NGX_D3D11_Shutdown1");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(evaluate, nullptr);
    ASSERT_NE(init, nullptr);
    ASSERT_NE(initExt, nullptr);
    ASSERT_NE(release, nullptr);
    ASSERT_NE(shutdown, nullptr);
    ASSERT_NE(shutdownDevice, nullptr);

    EXPECT_EQ(create(nullptr, nullptr, nullptr, nullptr), NgxUnsupported);
    EXPECT_EQ(evaluate(nullptr, nullptr, nullptr), NgxUnsupported);
    EXPECT_EQ(init(nullptr, nullptr, nullptr, 0), NgxUnsupported);
    EXPECT_EQ(initExt(nullptr, nullptr, nullptr, 0), NgxUnsupported);
    EXPECT_EQ(release(nullptr), NgxUnsupported);
    EXPECT_EQ(shutdown(), NgxUnsupported);
    EXPECT_EQ(shutdownDevice(nullptr), NgxUnsupported);
}

TEST(NgxAbi, D3D11RequirementsMatchAbi)
{
    ASSERT_NE(module().handle(), nullptr);

    using RequirementsFunction = NgxResult (*)(void *, void *, NgxFeatureRequirementInfo *);
    const auto requirements = requireExport<RequirementsFunction>("NVSDK_NGX_D3D11_GetFeatureRequirements");
    ASSERT_NE(requirements, nullptr);
    EXPECT_EQ(requirements(nullptr, nullptr, nullptr), NgxInvalidParameter);

    NgxFeatureRequirementInfo info = {};
    EXPECT_EQ(requirements(nullptr, &info, &info), NgxSuccess);
    EXPECT_EQ(info.flags, 0u);
    EXPECT_EQ(info.requiredGpuArchitecture, NgxGpuArchitecture);
    EXPECT_EQ(std::string_view(info.requiredOperatingSystemVersion), "10.0.0");
}

TEST(NgxAbi, D3D12RejectsSafeNullArguments)
{
    ASSERT_NE(module().handle(), nullptr);

    using CreateFunction = NgxResult (*)(void *, void *, void *, NgxHandle **);
    using EvaluateFunction = NgxResult (*)(void *, NgxHandle *, void *);
    using ScratchFunction = NgxResult (*)(void *, void *, std::uint64_t *);
    using InitFunction = NgxResult (*)(void *, const wchar_t *, void *, std::uint32_t);
    using ReleaseFunction = NgxResult (*)(NgxHandle *);
    using ShutdownDeviceFunction = NgxResult (*)(void *);

    const auto create = requireExport<CreateFunction>("NVSDK_NGX_D3D12_CreateFeature");
    const auto evaluate = requireExport<EvaluateFunction>("NVSDK_NGX_D3D12_EvaluateFeature");
    const auto scratch = requireExport<ScratchFunction>("NVSDK_NGX_D3D12_GetScratchBufferSize");
    const auto init = requireExport<InitFunction>("NVSDK_NGX_D3D12_Init");
    const auto release = requireExport<ReleaseFunction>("NVSDK_NGX_D3D12_ReleaseFeature");
    const auto shutdownDevice = requireExport<ShutdownDeviceFunction>("NVSDK_NGX_D3D12_Shutdown1");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(evaluate, nullptr);
    ASSERT_NE(scratch, nullptr);
    ASSERT_NE(init, nullptr);
    ASSERT_NE(release, nullptr);
    ASSERT_NE(shutdownDevice, nullptr);

    EXPECT_EQ(create(nullptr, nullptr, nullptr, nullptr), NgxInvalidParameter);
    EXPECT_EQ(evaluate(nullptr, nullptr, nullptr), NgxInvalidParameter);
    EXPECT_EQ(scratch(nullptr, nullptr, nullptr), NgxInvalidParameter);
    EXPECT_EQ(init(nullptr, nullptr, nullptr, 0), NgxInvalidParameter);
    EXPECT_EQ(release(nullptr), NgxInvalidParameter);
    EXPECT_EQ(shutdownDevice(nullptr), NgxInvalidParameter);
}

TEST(NgxAbi, VulkanRejectsSafeNullArguments)
{
    ASSERT_NE(module().handle(), nullptr);

    using CreateFunction = NgxResult (*)(void *, void *, void *, void *, NgxHandle **);
    using EvaluateFunction = NgxResult (*)(void *, NgxHandle *, void *);
    using RequirementsFunction = NgxResult (*)(void *, void *, void *, NgxFeatureRequirementInfo *);
    using InitFunction = NgxResult (*)(void *, void *, void *, void *, void *, std::uint32_t);
    using ReleaseFunction = NgxResult (*)(NgxHandle *);
    using ShutdownDeviceFunction = NgxResult (*)(void *);

    const auto create = requireExport<CreateFunction>("NVSDK_NGX_VULKAN_CreateFeature1");
    const auto evaluate = requireExport<EvaluateFunction>("NVSDK_NGX_VULKAN_EvaluateFeature");
    const auto requirements = requireExport<RequirementsFunction>("NVSDK_NGX_VULKAN_GetFeatureRequirements");
    const auto init = requireExport<InitFunction>("NVSDK_NGX_VULKAN_Init");
    const auto release = requireExport<ReleaseFunction>("NVSDK_NGX_VULKAN_ReleaseFeature");
    const auto shutdownDevice = requireExport<ShutdownDeviceFunction>("NVSDK_NGX_VULKAN_Shutdown1");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(evaluate, nullptr);
    ASSERT_NE(requirements, nullptr);
    ASSERT_NE(init, nullptr);
    ASSERT_NE(release, nullptr);
    ASSERT_NE(shutdownDevice, nullptr);

    EXPECT_EQ(create(nullptr, nullptr, nullptr, nullptr, nullptr), NgxInvalidParameter);
    EXPECT_EQ(evaluate(nullptr, nullptr, nullptr), NgxInvalidParameter);
    EXPECT_EQ(requirements(nullptr, nullptr, nullptr, nullptr), NgxInvalidParameter);
    EXPECT_EQ(init(nullptr, nullptr, nullptr, nullptr, nullptr, 0), NgxInvalidParameter);
    EXPECT_EQ(release(nullptr), NgxInvalidParameter);
    EXPECT_EQ(shutdownDevice(nullptr), NgxInvalidParameter);
}
}
