#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../../support/ngx_parameters.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>

namespace dx12_ngx_e2e::support_adapter
{
	using NgxResult = test_support::NgxResult;
	using NgxHandle = test_support::NgxHandle;
	using NgxInstanceParameters = test_support::NgxInstanceParameters;

	inline constexpr NgxResult kSuccess = test_support::ngx_success;

	struct NgxExports
	{
		using Init = NgxResult (*)(void *, const wchar_t *, ID3D12Device *, std::uint32_t);
		using PopulateDeviceParameters = NgxResult (*)(ID3D12Device *, NgxInstanceParameters *);
		using PopulateParameters = NgxResult (*)(NgxInstanceParameters *);
		using CreateFeature = NgxResult (*)(ID3D12CommandList *, void *, NgxInstanceParameters *, NgxHandle **);
		using EvaluateFeature = NgxResult (*)(ID3D12GraphicsCommandList *, NgxHandle *, NgxInstanceParameters *);
		using ReleaseFeature = NgxResult (*)(NgxHandle *);
		using Shutdown = NgxResult (*)();

		Init init = nullptr;
		PopulateDeviceParameters populateDeviceParameters = nullptr;
		PopulateParameters populateParameters = nullptr;
		CreateFeature createFeature = nullptr;
		EvaluateFeature evaluateFeature = nullptr;
		ReleaseFeature releaseFeature = nullptr;
		Shutdown shutdown = nullptr;
	};
}
