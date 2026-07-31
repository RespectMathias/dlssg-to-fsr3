#pragma once

#include "FFFrameInterpolator.h"

struct ID3D12Device;

class FFFrameInterpolatorDX final : public FFFrameInterpolator
{
private:
	ID3D12Device *const m_Device;
	void *m_ActiveCommandList = nullptr;

public:
	FFFrameInterpolatorDX(ID3D12Device *Device, uint32_t OutputWidth, uint32_t OutputHeight, NGXInstanceParameters *NGXParameters);
	FFFrameInterpolatorDX(const FFFrameInterpolatorDX&) = delete;
	FFFrameInterpolatorDX& operator=(const FFFrameInterpolatorDX&) = delete;
	~FFFrameInterpolatorDX();

	FrameGenerationStatus Dispatch(void *CommandList, NGXInstanceParameters *NGXParameters) override;

private:
	std::array<uint8_t, 8> GetActiveAdapterLUID() const override;
	void *GetActiveCommandList() const override;

	void CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source) override;

	bool LoadTextureFromNGXParameters(
		NGXInstanceParameters *NGXParameters,
		const char *Name,
		FfxApiResource *OutFfxResource,
		uint32_t State) override;
};
