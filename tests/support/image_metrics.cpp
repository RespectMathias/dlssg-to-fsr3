#include "image_metrics.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace test_support
{
	ImageMetrics ComputeImageMetrics(std::span<const std::uint8_t> actual_rgba8,
									std::span<const std::uint8_t> reference_rgba8,
									std::uint32_t width,
									std::uint32_t height)
	{
		const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
		if (width == 0U || height == 0U || actual_rgba8.size() != pixel_count * 4U || reference_rgba8.size() != pixel_count * 4U)
		{
			throw std::invalid_argument("Image metrics require equally sized non-empty RGBA8 images");
		}

		double absolute_sum = 0.0;
		double squared_sum = 0.0;
		double weighted_x = 0.0;
		double weighted_y = 0.0;
		double error_weight = 0.0;
		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
				double pixel_error = 0.0;
				for (std::size_t channel = 0; channel < 3U; ++channel)
				{
					const double difference = static_cast<double>(actual_rgba8[pixel * 4U + channel]) -
											  static_cast<double>(reference_rgba8[pixel * 4U + channel]);
					const double absolute = std::abs(difference);
					absolute_sum += absolute;
					squared_sum += difference * difference;
					pixel_error += absolute;
				}
				weighted_x += pixel_error * static_cast<double>(x);
				weighted_y += pixel_error * static_cast<double>(y);
				error_weight += pixel_error;
			}
		}

		const double sample_count = static_cast<double>(pixel_count) * 3.0;
		const double mean_squared_error = squared_sum / sample_count;
		ImageMetrics result;
		result.mae = absolute_sum / sample_count;
		result.psnr = mean_squared_error == 0.0 ? std::numeric_limits<double>::infinity()
												 : 10.0 * std::log10((255.0 * 255.0) / mean_squared_error);
		if (error_weight == 0.0)
		{
			result.error_centroid_x = (static_cast<double>(width) - 1.0) * 0.5;
			result.error_centroid_y = (static_cast<double>(height) - 1.0) * 0.5;
		}
		else
		{
			result.error_centroid_x = weighted_x / error_weight;
			result.error_centroid_y = weighted_y / error_weight;
		}
		return result;
	}
}
