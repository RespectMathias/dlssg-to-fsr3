#pragma once

#include <cstdint>
#include <span>

namespace test_support
{
	struct ImageMetrics
	{
		double mae = 0.0;
		double psnr = 0.0;
		double error_centroid_x = 0.0;
		double error_centroid_y = 0.0;
	};

	[[nodiscard]] ImageMetrics ComputeImageMetrics(std::span<const std::uint8_t> actual_rgba8,
													   std::span<const std::uint8_t> reference_rgba8,
													   std::uint32_t width,
													   std::uint32_t height);
}
