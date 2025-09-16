#pragma once

#include "FFFrameInterpolator.h"

struct ID3D12Device;

class FFFrameInterpolatorDX final : public FFFrameInterpolator
{
private:
	ID3D12Device *const m_Device;

	// Transient
	FfxCommandList m_ActiveCommandList = {};

	// Optional HDR conversion pipeline (built on demand)
	ID3D12RootSignature *m_PqToScRgbRootSig = nullptr;
	ID3D12PipelineState *m_PqToScRgbPso = nullptr;
	ID3D12DescriptorHeap *m_ConvertHeap = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE m_ConvertSrvCpu = {};
	D3D12_CPU_DESCRIPTOR_HANDLE m_ConvertUavCpu = {};
	D3D12_GPU_DESCRIPTOR_HANDLE m_ConvertHeapGpuStart = {};
	UINT m_ConvertDescriptorSize = 0;
	ID3D12Resource *m_ConvertedBackbuffer = nullptr;
	UINT m_ConvertedWidth = 0;
	UINT m_ConvertedHeight = 0;

public:
	FFFrameInterpolatorDX(ID3D12Device *Device, uint32_t OutputWidth, uint32_t OutputHeight, NGXInstanceParameters *NGXParameters);
	FFFrameInterpolatorDX(const FFFrameInterpolatorDX&) = delete;
	FFFrameInterpolatorDX& operator=(const FFFrameInterpolatorDX&) = delete;
	~FFFrameInterpolatorDX();

	FfxErrorCode Dispatch(void *CommandList, NGXInstanceParameters *NGXParameters) override;

private:
	FfxErrorCode InitializeBackendInterface(
		FFInterfaceWrapper *BackendInterface,
		uint32_t MaxContexts,
		NGXInstanceParameters *NGXParameters) override;

	std::array<uint8_t, 8> GetActiveAdapterLUID() const override;
	FfxCommandList GetActiveCommandList() const override;

	void CopyTexture(FfxCommandList CommandList, const FfxResource *Destination, const FfxResource *Source) override;

	bool LoadTextureFromNGXParameters(
		NGXInstanceParameters *NGXParameters,
		const char *Name,
		FfxResource *OutFfxResource,
		FfxResourceStates State) override;

protected:
	bool OnBeforeDispatchAdjustParams(FFInterpolatorDispatchParameters& params) override;

private:
	bool EnsureConversionPipeline();
	bool EnsureConvertedTarget(UINT width, UINT height);
	bool RunPqToScRgbConversion(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src);
};
