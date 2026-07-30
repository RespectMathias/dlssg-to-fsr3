#pragma once

#include "FFInterpolator.h"

struct NGXInstanceParameters;

class FFFrameInterpolator
{
private:
	std::optional<FFInterpolator> m_FrameInterpolatorContext;

	const uint32_t m_SwapchainWidth;
	const uint32_t m_SwapchainHeight;

	FfxApiFloatCoords2D m_HDRLuminanceRange = { 0.0001f, 1000.0f };
	bool m_HDRLuminanceRangeSet = false;

	uint32_t m_PreUpscaleRenderWidth = 0;
	uint32_t m_PreUpscaleRenderHeight = 0;
	uint32_t m_PostUpscaleRenderWidth = 0;
	uint32_t m_PostUpscaleRenderHeight = 0;

public:
	FFFrameInterpolator(uint32_t OutputWidth, uint32_t OutputHeight);
	FFFrameInterpolator(const FFFrameInterpolator&) = delete;
	FFFrameInterpolator& operator=(const FFFrameInterpolator&) = delete;
	~FFFrameInterpolator();

	virtual FrameGenerationStatus Dispatch(void *CommandList, NGXInstanceParameters *NGXParameters);

protected:
	virtual std::array<uint8_t, 8> GetActiveAdapterLUID() const = 0;
	virtual void *GetActiveCommandList() const = 0;

	virtual void CopyTexture(void *CommandList, const FfxApiResource *Destination, const FfxApiResource *Source) = 0;

	virtual bool LoadTextureFromNGXParameters(
		NGXInstanceParameters *NGXParameters,
		const char *Name,
		FfxApiResource *OutFfxResource,
		uint32_t State) = 0;

	void Create(ID3D12Device *Device, NGXInstanceParameters *NGXParameters);
	void Destroy();

private:
	bool CalculateResourceDimensions(NGXInstanceParameters *NGXParameters);
	void QueryHDRLuminanceRange(NGXInstanceParameters *NGXParameters);
	bool BuildFrameInterpolationParameters(FFInterpolatorDispatchParameters *OutParameters, NGXInstanceParameters *NGXParameters);
};
