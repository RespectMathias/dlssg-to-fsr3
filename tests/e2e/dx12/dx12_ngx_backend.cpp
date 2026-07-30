#include "dx12_ngx_backend.h"

#include "support_adapter.h"
#include "../../support/procedural_scene.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dx12_ngx_e2e
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using support_adapter::NgxExports;
		using support_adapter::NgxHandle;
		using support_adapter::NgxInstanceParameters;
		using support_adapter::NgxResult;

		constexpr std::uint32_t kColorBytesPerPixel = 4;
		constexpr std::uint32_t kDepthBytesPerPixel = 4;
		constexpr std::uint32_t kMotionBytesPerPixel = 4;

		void RejectResourceAllocation(
			D3D12_RESOURCE_DESC *,
			std::uint32_t,
			D3D12_HEAP_PROPERTIES *,
			ID3D12Resource **resource)
		{
			*resource = nullptr;
		}

		void RejectResourceRelease(ID3D12Resource *)
		{
		}

		void CheckHr(HRESULT result, std::string_view operation)
		{
			if (FAILED(result))
				throw std::runtime_error(std::format("{} failed with HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result)));
		}

		void CheckNgx(NgxResult result, std::string_view operation)
		{
			if (result != support_adapter::kSuccess)
				throw std::runtime_error(std::format("{} failed with NGX result 0x{:08X}", operation, result));
		}

		class Module
		{
		public:
			explicit Module(const std::wstring& path)
			{
				const auto absolutePath = std::filesystem::absolute(path);
				handle_ = LoadLibraryExW(
					absolutePath.c_str(),
					nullptr,
					LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				if (handle_ == nullptr)
					throw std::runtime_error(
						std::format("LoadLibraryExW failed for {} with error {}", absolutePath.string(), GetLastError()));

				try
				{
					exports_.init = Load<NgxExports::Init>("NVSDK_NGX_D3D12_Init");
					exports_.populateDeviceParameters = Load<NgxExports::PopulateDeviceParameters>(
						"NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl");
					exports_.populateParameters = Load<NgxExports::PopulateParameters>("NVSDK_NGX_D3D12_PopulateParameters_Impl");
					exports_.createFeature = Load<NgxExports::CreateFeature>("NVSDK_NGX_D3D12_CreateFeature");
					exports_.evaluateFeature = Load<NgxExports::EvaluateFeature>("NVSDK_NGX_D3D12_EvaluateFeature");
					exports_.releaseFeature = Load<NgxExports::ReleaseFeature>("NVSDK_NGX_D3D12_ReleaseFeature");
					exports_.shutdown = Load<NgxExports::Shutdown>("NVSDK_NGX_D3D12_Shutdown");
				}
				catch (...)
				{
					FreeLibrary(handle_);
					handle_ = nullptr;
					throw;
				}
			}

			~Module()
			{
				if (handle_ != nullptr)
					FreeLibrary(handle_);
			}

			Module(const Module&) = delete;
			Module& operator=(const Module&) = delete;

			const NgxExports& Exports() const noexcept
			{
				return exports_;
			}

		private:
			template<typename T>
			T Load(const char *name)
			{
				const FARPROC address = GetProcAddress(handle_, name);
				if (address == nullptr)
					throw std::runtime_error(std::format("DLL does not export {}", name));
				static_assert(sizeof(T) == sizeof(address));
				return std::bit_cast<T>(address);
			}

			HMODULE handle_ = nullptr;
			NgxExports exports_ {};
		};

		struct Texture
		{
			ComPtr<ID3D12Resource> resource;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t bytesPerPixel = 0;
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		};

		std::vector<std::uint8_t> MakeSceneColor(std::uint32_t width, std::uint32_t height, std::uint32_t frameIndex)
		{
			std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * kColorBytesPerPixel);
			const std::uint32_t shift = (frameIndex * 5U) % width;
			for (std::uint32_t y = 0; y < height; ++y)
			{
				for (std::uint32_t x = 0; x < width; ++x)
				{
					const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * kColorBytesPerPixel;
					const std::uint32_t movedX = (x + shift) % width;
					const bool checker = ((movedX / 8U) + (y / 8U)) % 2U != 0;
					result[offset] = static_cast<std::uint8_t>((movedX * 251U / width + (checker ? 37U : 0U)) & 0xFFU);
					result[offset + 1] = static_cast<std::uint8_t>((y * 239U / height + (checker ? 71U : 11U)) & 0xFFU);
					result[offset + 2] = checker ? 224U : 32U;
					result[offset + 3] = 255U;
				}
			}
			return result;
		}

		std::vector<float> MakeDepth(std::uint32_t width, std::uint32_t height)
		{
			std::vector<float> result(static_cast<std::size_t>(width) * height);
			for (std::uint32_t y = 0; y < height; ++y)
			{
				for (std::uint32_t x = 0; x < width; ++x)
					result[static_cast<std::size_t>(y) * width + x] = 0.1F + 0.8F * static_cast<float>(x + y) /
																				 static_cast<float>(width + height - 2U);
			}
			return result;
		}

		std::vector<std::uint16_t> MakeMotion(std::uint32_t width, std::uint32_t height)
		{
			std::vector<std::uint16_t> result(static_cast<std::size_t>(width) * height * 2U);
			for (std::size_t index = 0; index < result.size(); index += 2U)
			{
				result[index] = test_support::FloatToHalf(-5.0F / static_cast<float>(width));
				result[index + 1U] = test_support::FloatToHalf(0.0F);
			}
			return result;
		}
	}

	class Dx12NgxBackend::Impl
	{
	public:
		explicit Impl(BackendConfig config) : config_(std::move(config)), module_(config_.dll)
		{
			if (config_.dll.empty())
				throw std::invalid_argument("dll is empty");
			if (config_.width <= 32U || config_.height <= 32U)
				throw std::invalid_argument("DX12 NGX dimensions must both exceed 32");
			if (config_.fenceTimeout <= std::chrono::milliseconds::zero())
				throw std::invalid_argument("fenceTimeout must be positive");
		}

		void Initialize()
		{
			CreateDevice();
			CreateQueueObjects();
			CreateTextures();
			InitializeNgx();
		}

		~Impl()
		{
			if (feature_ != nullptr)
				static_cast<void>(module_.Exports().releaseFeature(feature_));
			if (initialized_)
				static_cast<void>(module_.Exports().shutdown());
			if (fenceEvent_ != nullptr)
				CloseHandle(fenceEvent_);
		}

		FrameResult RunFrame(std::uint32_t frameIndex, bool enableInterpolation, bool reset, RecordingPath recordingPath)
		{
			FrameInput input;
			input.colorRgba8 = MakeSceneColor(config_.width, config_.height, frameIndex);
			input.depth = MakeDepth(config_.width, config_.height);
			input.motionRg16f = MakeMotion(config_.width, config_.height);
			return RunFrame(input, enableInterpolation, reset, recordingPath);
		}

		FrameResult RunFrame(const FrameInput& input, bool enableInterpolation, bool reset, RecordingPath recordingPath)
		{
			const auto totalStart = std::chrono::steady_clock::now();
			FrameResult result;
			result.width = config_.width;
			result.height = config_.height;
			const auto pixelCount = static_cast<std::size_t>(config_.width) * config_.height;
			if (input.colorRgba8.size() != pixelCount * 4 || input.depth.size() != pixelCount || input.motionRg16f.size() != pixelCount * 2)
				throw std::invalid_argument("DX12 frame input sizes do not match configured dimensions");
			result.submittedColor = input.colorRgba8;
			result.interpolationBaseline.assign(result.submittedColor.size(), 0xCDU);

			const auto uploadStart = std::chrono::steady_clock::now();
			BeginCommandList();
			UploadTexture(backbuffer_, std::as_bytes(std::span(result.submittedColor)), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			UploadTexture(depth_, std::as_bytes(std::span(input.depth)), D3D12_RESOURCE_STATE_COPY_DEST);
			UploadTexture(motion_, std::as_bytes(std::span(input.motionRg16f)), D3D12_RESOURCE_STATE_COPY_DEST);
			if (hudless_.resource)
			{
				const std::vector<std::uint16_t> hudless(
					static_cast<std::size_t>(config_.width) * config_.height * 4U,
					test_support::FloatToHalf(0.25F));
				UploadTexture(hudless_, std::as_bytes(std::span(hudless)), D3D12_RESOURCE_STATE_COPY_DEST);
			}
			UploadTexture(outputReal_, std::as_bytes(std::span(result.interpolationBaseline)), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			UploadTexture(
				outputInterpolated_,
				std::as_bytes(std::span(result.interpolationBaseline)),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			SubmitAndWait();
			result.timings.upload = Elapsed(uploadStart);

			SetFrameParameters(enableInterpolation, reset, recordingPath);
			CheckHr(allocator_->Reset(), "ID3D12CommandAllocator::Reset for evaluation");

			if (recordingPath == RecordingPath::callerOwned)
				CheckHr(commandList_->Reset(allocator_.Get(), nullptr), "ID3D12GraphicsCommandList::Reset for caller-owned evaluation");

			const auto evaluateStart = std::chrono::steady_clock::now();
			CheckNgx(module_.Exports().evaluateFeature(commandList_.Get(), feature_, &parameters_), "NVSDK_NGX_D3D12_EvaluateFeature");
			result.timings.evaluateCall = Elapsed(evaluateStart);

			if (recordingPath == RecordingPath::callerOwned)
				CheckHr(commandList_->Close(), "ID3D12GraphicsCommandList::Close after caller-owned evaluation");

			const auto completionStart = std::chrono::steady_clock::now();
			ExecuteClosedCommandListAndWait();
			result.timings.gpuCompletion = Elapsed(completionStart);

			const auto readbackStart = std::chrono::steady_clock::now();
			result.outputReal = Readback(outputReal_);
			result.outputInterpolated = Readback(outputInterpolated_);
			result.timings.readback = Elapsed(readbackStart);
			result.timings.total = Elapsed(totalStart);
			timingHistory_.push_back(result.timings);
			return result;
		}

		FrameResult WarmupInterpolation(std::uint32_t frameIndex, RecordingPath recordingPath)
		{
			static_cast<void>(RunFrame(frameIndex, false, true, recordingPath));
			return RunFrame(frameIndex, true, true, recordingPath);
		}

		const std::vector<FrameTimings>& TimingHistory() const noexcept
		{
			return timingHistory_;
		}

	private:
		static std::chrono::microseconds Elapsed(std::chrono::steady_clock::time_point start)
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
		}

		void CreateDevice()
		{
			ComPtr<IDXGIFactory6> factory;
			CheckHr(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory2");

			for (UINT index = 0;; ++index)
			{
				ComPtr<IDXGIAdapter1> candidate;
				const HRESULT enumerateResult = factory->EnumAdapterByGpuPreference(
					index,
					DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
					IID_PPV_ARGS(candidate.GetAddressOf()));
				if (enumerateResult == DXGI_ERROR_NOT_FOUND)
					break;
				CheckHr(enumerateResult, "IDXGIFactory6::EnumAdapterByGpuPreference");

				DXGI_ADAPTER_DESC1 description {};
				CheckHr(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
				if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U)
					continue;
				ComPtr<ID3D12Device> candidateDevice;
				if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(candidateDevice.GetAddressOf()))))
				{
					device_ = std::move(candidateDevice);
					return;
				}
			}

			if (config_.allowWarp)
			{
				ComPtr<IDXGIAdapter> warp;
				CheckHr(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf())), "IDXGIFactory6::EnumWarpAdapter");
				CheckHr(
					D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device_.GetAddressOf())),
					"D3D12CreateDevice WARP");
				return;
			}

			throw std::runtime_error("No hardware D3D12 feature-level 12_0 adapter found");
		}

		void CreateQueueObjects()
		{
			const D3D12_COMMAND_QUEUE_DESC queueDescription {
				.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
				.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
				.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
				.NodeMask = 0,
			};
			CheckHr(
				device_->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(queue_.GetAddressOf())),
				"ID3D12Device::CreateCommandQueue");
			CheckHr(
				device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator_.GetAddressOf())),
				"ID3D12Device::CreateCommandAllocator");
			CheckHr(
				device_->CreateCommandList(
					0,
					D3D12_COMMAND_LIST_TYPE_DIRECT,
					allocator_.Get(),
					nullptr,
					IID_PPV_ARGS(commandList_.GetAddressOf())),
				"ID3D12Device::CreateCommandList");
			CheckHr(commandList_->Close(), "Initial ID3D12GraphicsCommandList::Close");
			CheckHr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf())), "ID3D12Device::CreateFence");
			fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (fenceEvent_ == nullptr)
				throw std::runtime_error(std::format("CreateEventW failed with error {}", GetLastError()));
		}

		Texture CreateTexture(DXGI_FORMAT format, std::uint32_t bytesPerPixel, D3D12_RESOURCE_FLAGS flags)
		{
			Texture result;
			result.format = format;
			result.bytesPerPixel = bytesPerPixel;
			result.state = D3D12_RESOURCE_STATE_COPY_DEST;

			const D3D12_HEAP_PROPERTIES heap {
				.Type = D3D12_HEAP_TYPE_DEFAULT,
				.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
				.CreationNodeMask = 1,
				.VisibleNodeMask = 1,
			};
			const D3D12_RESOURCE_DESC description {
				.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
				.Alignment = 0,
				.Width = config_.width,
				.Height = config_.height,
				.DepthOrArraySize = 1,
				.MipLevels = 1,
				.Format = format,
				.SampleDesc = { .Count = 1, .Quality = 0 },
				.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
				.Flags = flags,
			};
			CheckHr(
				device_->CreateCommittedResource(
					&heap,
					D3D12_HEAP_FLAG_NONE,
					&description,
					result.state,
					nullptr,
					IID_PPV_ARGS(result.resource.GetAddressOf())),
				"ID3D12Device::CreateCommittedResource texture");
			return result;
		}

		void CreateTextures()
		{
			backbuffer_ = CreateTexture(DXGI_FORMAT_R8G8B8A8_UNORM, kColorBytesPerPixel, D3D12_RESOURCE_FLAG_NONE);
			depth_ = CreateTexture(DXGI_FORMAT_R32_FLOAT, kDepthBytesPerPixel, D3D12_RESOURCE_FLAG_NONE);
			motion_ = CreateTexture(DXGI_FORMAT_R16G16_FLOAT, kMotionBytesPerPixel, D3D12_RESOURCE_FLAG_NONE);
			outputReal_ = CreateTexture(DXGI_FORMAT_R8G8B8A8_UNORM, kColorBytesPerPixel, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			outputInterpolated_ = CreateTexture(
				DXGI_FORMAT_R8G8B8A8_UNORM,
				kColorBytesPerPixel,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			if (config_.provideIncompatibleHudless)
				hudless_ = CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 8, D3D12_RESOURCE_FLAG_NONE);
		}

		void InitializeNgx()
		{
			CheckNgx(module_.Exports().init(nullptr, L".", device_.Get(), 0), "NVSDK_NGX_D3D12_Init");
			initialized_ = true;
			CheckNgx(
				module_.Exports().populateDeviceParameters(device_.Get(), &parameters_),
				"NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl");
			CheckNgx(module_.Exports().populateParameters(&parameters_), "NVSDK_NGX_D3D12_PopulateParameters_Impl");

			parameters_.Set5("Width", config_.width);
			parameters_.Set5("Height", config_.height);
			parameters_.Set5("DLSSG.MultiFrameCount", 1);
			parameters_.Set5("DLSSG.MultiFrameIndex", 1);
			parameters_.Set5("DLSSG.ColorBuffersHDR", 0);
			parameters_.Set5("DLSSG.DepthInverted", 0);
			parameters_.Set5("DLSSG.MvecJittered", 0);
			parameters_.Set5("DLSSG.MvecDilated", config_.motionVectorsDilated ? 1U : 0U);
			parameters_.Set5("DLSSG.MVecsSubrectWidth", config_.width);
			parameters_.Set5("DLSSG.MVecsSubrectHeight", config_.height);
			parameters_.Set5("DLSSG.DepthSubrectWidth", config_.width);
			parameters_.Set5("DLSSG.DepthSubrectHeight", config_.height);
			parameters_.Set2("DLSSG.MvecScaleX", static_cast<float>(config_.width));
			parameters_.Set2("DLSSG.MvecScaleY", static_cast<float>(config_.height));
			parameters_.Set2("DLSSG.JitterOffsetX", 0.0F);
			parameters_.Set2("DLSSG.JitterOffsetY", 0.0F);
			parameters_.Set2("DLSSG.CameraFOV", 1.04719755F);
			parameters_.Set2("DLSSG.CameraNear", 0.1F);
			parameters_.Set2("DLSSG.CameraFar", 1000.0F);
			parameters_.SetVoidPointer("DLSSG.Backbuffer", backbuffer_.resource.Get());
			parameters_.SetVoidPointer("DLSSG.Depth", depth_.resource.Get());
			parameters_.SetVoidPointer("DLSSG.MVecs", motion_.resource.Get());
			parameters_.SetVoidPointer("DLSSG.OutputReal", outputReal_.resource.Get());
			parameters_.SetVoidPointer("DLSSG.OutputInterpolated", outputInterpolated_.resource.Get());
			if (hudless_.resource)
				parameters_.SetVoidPointer("DLSSG.HUDLess", hudless_.resource.Get());
			if (config_.provideRejectingResourceCallbacks)
			{
				parameters_.SetVoidPointer("ResourceAllocCallback", reinterpret_cast<void *>(&RejectResourceAllocation));
				parameters_.SetVoidPointer("ResourceReleaseCallback", reinterpret_cast<void *>(&RejectResourceRelease));
			}

			CheckNgx(
				module_.Exports().createFeature(commandList_.Get(), nullptr, &parameters_, &feature_),
				"NVSDK_NGX_D3D12_CreateFeature");
			if (feature_ == nullptr)
				throw std::runtime_error("NVSDK_NGX_D3D12_CreateFeature returned a null handle");
		}

		void SetFrameParameters(bool enableInterpolation, bool reset, RecordingPath recordingPath)
		{
			parameters_.Set5("DLSSG.EnableInterp", enableInterpolation ? 1U : 0U);
			parameters_.Set5("DLSSG.Reset", reset ? 1U : 0U);
			parameters_.Set5("DLSSG.IsRecording", recordingPath == RecordingPath::callerOwned ? 1U : 0U);
			parameters_.SetVoidPointer("DLSSG.CmdQueue", queue_.Get());
			parameters_.SetVoidPointer("DLSSG.CmdAlloc", allocator_.Get());
		}

		void BeginCommandList()
		{
			CheckHr(allocator_->Reset(), "ID3D12CommandAllocator::Reset");
			CheckHr(commandList_->Reset(allocator_.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
			transientUploads_.clear();
		}

		ComPtr<ID3D12Resource> CreateBuffer(std::uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state)
		{
			const D3D12_HEAP_PROPERTIES heap {
				.Type = heapType,
				.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
				.CreationNodeMask = 1,
				.VisibleNodeMask = 1,
			};
			const D3D12_RESOURCE_DESC description {
				.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
				.Alignment = 0,
				.Width = size,
				.Height = 1,
				.DepthOrArraySize = 1,
				.MipLevels = 1,
				.Format = DXGI_FORMAT_UNKNOWN,
				.SampleDesc = { .Count = 1, .Quality = 0 },
				.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
				.Flags = D3D12_RESOURCE_FLAG_NONE,
			};
			ComPtr<ID3D12Resource> result;
			CheckHr(
				device_->CreateCommittedResource(
					&heap,
					D3D12_HEAP_FLAG_NONE,
					&description,
					state,
					nullptr,
					IID_PPV_ARGS(result.GetAddressOf())),
				"ID3D12Device::CreateCommittedResource buffer");
			return result;
		}

		void Transition(Texture& texture, D3D12_RESOURCE_STATES target)
		{
			if (texture.state == target)
				return;
			const D3D12_RESOURCE_BARRIER barrier{
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {
                .pResource = texture.resource.Get(),
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = texture.state,
                .StateAfter = target,
            },
        };
			commandList_->ResourceBarrier(1, &barrier);
			texture.state = target;
		}

		void UploadTexture(Texture& texture, std::span<const std::byte> bytes, D3D12_RESOURCE_STATES finalState)
		{
			const std::size_t sourceRowSize = static_cast<std::size_t>(config_.width) * texture.bytesPerPixel;
			const std::size_t requiredSize = sourceRowSize * config_.height;
			if (bytes.size() != requiredSize)
				throw std::invalid_argument("UploadTexture byte count does not match texture");

			Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
			const D3D12_RESOURCE_DESC description = texture.resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
			UINT rowCount = 0;
			UINT64 rowSize = 0;
			UINT64 totalSize = 0;
			device_->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rowCount, &rowSize, &totalSize);
			if (rowCount != config_.height || rowSize != sourceRowSize)
				throw std::runtime_error("Unexpected D3D12 upload footprint");

			auto upload = CreateBuffer(totalSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
			std::byte *mapped = nullptr;
			const D3D12_RANGE noRead { 0, 0 };
			CheckHr(upload->Map(0, &noRead, reinterpret_cast<void **>(&mapped)), "ID3D12Resource::Map upload");
			for (std::uint32_t row = 0; row < config_.height; ++row)
			{
				std::memcpy(
					mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
					bytes.data() + static_cast<std::size_t>(row) * sourceRowSize,
					sourceRowSize);
			}
			upload->Unmap(0, nullptr);

			const D3D12_TEXTURE_COPY_LOCATION destination {
				.pResource = texture.resource.Get(),
				.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
				.SubresourceIndex = 0,
			};
			const D3D12_TEXTURE_COPY_LOCATION source {
				.pResource = upload.Get(),
				.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
				.PlacedFootprint = footprint,
			};
			commandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			Transition(texture, finalState);
			transientUploads_.push_back(std::move(upload));
		}

		std::vector<std::uint8_t> Readback(Texture& texture)
		{
			BeginCommandList();
			Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);

			const D3D12_RESOURCE_DESC description = texture.resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
			UINT rowCount = 0;
			UINT64 rowSize = 0;
			UINT64 totalSize = 0;
			device_->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rowCount, &rowSize, &totalSize);
			auto readback = CreateBuffer(totalSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

			const D3D12_TEXTURE_COPY_LOCATION destination {
				.pResource = readback.Get(),
				.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
				.PlacedFootprint = footprint,
			};
			const D3D12_TEXTURE_COPY_LOCATION source {
				.pResource = texture.resource.Get(),
				.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
				.SubresourceIndex = 0,
			};
			commandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			SubmitAndWait();

			const std::size_t outputRowSize = static_cast<std::size_t>(config_.width) * texture.bytesPerPixel;
			std::vector<std::uint8_t> result(outputRowSize * config_.height);
			void *mappedMemory = nullptr;
			const D3D12_RANGE readRange { 0, static_cast<SIZE_T>(totalSize) };
			CheckHr(readback->Map(0, &readRange, &mappedMemory), "ID3D12Resource::Map readback");
			const auto *mapped = static_cast<const std::byte *>(mappedMemory);
			for (std::uint32_t row = 0; row < config_.height; ++row)
			{
				std::memcpy(
					result.data() + static_cast<std::size_t>(row) * outputRowSize,
					mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
					outputRowSize);
			}
			const D3D12_RANGE noWrite { 0, 0 };
			readback->Unmap(0, &noWrite);
			return result;
		}

		void SubmitAndWait()
		{
			CheckHr(commandList_->Close(), "ID3D12GraphicsCommandList::Close");
			ExecuteClosedCommandListAndWait();
			transientUploads_.clear();
		}

		void ExecuteClosedCommandListAndWait()
		{
			ID3D12CommandList *lists[] = { commandList_.Get() };
			queue_->ExecuteCommandLists(1, lists);
			const std::uint64_t value = ++fenceValue_;
			CheckHr(queue_->Signal(fence_.Get(), value), "ID3D12CommandQueue::Signal");
			if (fence_->GetCompletedValue() >= value)
				return;
			CheckHr(fence_->SetEventOnCompletion(value, fenceEvent_), "ID3D12Fence::SetEventOnCompletion");
			const auto timeoutCount = config_.fenceTimeout.count();
			const DWORD timeout = static_cast<DWORD>(std::min<std::int64_t>(timeoutCount, std::numeric_limits<DWORD>::max() - 1LL));
			const DWORD waitResult = WaitForSingleObject(fenceEvent_, timeout);
			if (waitResult == WAIT_TIMEOUT)
			{
				WaitForSingleObject(fenceEvent_, INFINITE);
				throw std::runtime_error(std::format("D3D12 fence wait timed out after {} ms", timeout));
			}
			if (waitResult != WAIT_OBJECT_0)
				throw std::runtime_error(std::format("WaitForSingleObject failed with result {} and error {}", waitResult, GetLastError()));
		}

		BackendConfig config_;
		Module module_;
		test_support::NgxParameterBag parameters_;
		ComPtr<ID3D12Device> device_;
		ComPtr<ID3D12CommandQueue> queue_;
		ComPtr<ID3D12CommandAllocator> allocator_;
		ComPtr<ID3D12GraphicsCommandList> commandList_;
		ComPtr<ID3D12Fence> fence_;
		HANDLE fenceEvent_ = nullptr;
		std::uint64_t fenceValue_ = 0;
		Texture backbuffer_;
		Texture depth_;
		Texture motion_;
		Texture outputReal_;
		Texture outputInterpolated_;
		Texture hudless_;
		std::vector<ComPtr<ID3D12Resource>> transientUploads_;
		NgxHandle *feature_ = nullptr;
		bool initialized_ = false;
		std::vector<FrameTimings> timingHistory_;
	};

	BackendConfig BackendConfig::FromEnvironment()
	{
		BackendConfig result;
		const DWORD required = GetEnvironmentVariableW(L"DLSSG_TO_FSR3_DLL", nullptr, 0);
		if (required == 0)
			return result;
		std::wstring value(required, L'\0');
		const DWORD written = GetEnvironmentVariableW(L"DLSSG_TO_FSR3_DLL", value.data(), required);
		if (written == 0 || written >= required)
			throw std::runtime_error(std::format("GetEnvironmentVariableW failed with error {}", GetLastError()));
		value.resize(written);
		result.dll = std::move(value);
		return result;
	}

	std::unique_ptr<Dx12NgxBackend> Dx12NgxBackend::Create(const BackendConfig& config)
	{
		if (config.dll.empty())
			throw std::invalid_argument("Set DLSSG_TO_FSR3_DLL or BackendConfig::dll");
		auto impl = std::make_unique<Impl>(config);
		impl->Initialize();
		return std::unique_ptr<Dx12NgxBackend>(new Dx12NgxBackend(std::move(impl)));
	}

	Dx12NgxBackend::Dx12NgxBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
	Dx12NgxBackend::~Dx12NgxBackend() = default;

	FrameResult Dx12NgxBackend::RunFrame(std::uint32_t frameIndex, bool enableInterpolation, bool reset, RecordingPath recordingPath)
	{
		return impl_->RunFrame(frameIndex, enableInterpolation, reset, recordingPath);
	}

	FrameResult Dx12NgxBackend::RunFrame(
		const FrameInput& input,
		bool enableInterpolation,
		bool reset,
		RecordingPath recordingPath)
	{
		return impl_->RunFrame(input, enableInterpolation, reset, recordingPath);
	}

	FrameResult Dx12NgxBackend::WarmupInterpolation(std::uint32_t frameIndex, RecordingPath recordingPath)
	{
		return impl_->WarmupInterpolation(frameIndex, recordingPath);
	}

	const std::vector<FrameTimings>& Dx12NgxBackend::TimingHistory() const noexcept
	{
		return impl_->TimingHistory();
	}
}
