#include "procedural_scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace test_support
{
	namespace
	{
		std::uint8_t ToByte(float value)
		{
			return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
		}

		SceneFrame Render(float time)
		{
			SceneFrame frame;
			const std::size_t pixel_count = static_cast<std::size_t>(scene_width) * scene_height;
			frame.color_rgba8.resize(pixel_count * 4U);
			frame.depth.assign(pixel_count, 1.0F);
			frame.motion_rg16f.assign(pixel_count * 2U, FloatToHalf(0.0F));

			for (std::uint32_t y = 0; y < scene_height; ++y)
			{
				for (std::uint32_t x = 0; x < scene_width; ++x)
				{
					const std::size_t index = static_cast<std::size_t>(y) * scene_width + x;
					const float fx = static_cast<float>(x) / static_cast<float>(scene_width - 1U);
					const float fy = static_cast<float>(y) / static_cast<float>(scene_height - 1U);
					const float checker = ((x / 32U + y / 32U) & 1U) == 0U ? 0.035F : -0.035F;
					frame.color_rgba8[index * 4U + 0U] = ToByte(0.08F + 0.32F * fx + checker);
					frame.color_rgba8[index * 4U + 1U] = ToByte(0.12F + 0.28F * fy + checker);
					frame.color_rgba8[index * 4U + 2U] = ToByte(0.22F + 0.18F * (1.0F - fx) - checker);
					frame.color_rgba8[index * 4U + 3U] = 255U;
				}
			}

			auto draw_disc = [&](float center_x,
								 float center_y,
								 float radius,
								 float object_depth,
								 float velocity_x,
								 float velocity_y,
								 float red,
								 float green,
								 float blue)
			{
				const float radius_squared = radius * radius;
				for (std::uint32_t y = 0; y < scene_height; ++y)
				{
					for (std::uint32_t x = 0; x < scene_width; ++x)
					{
						const float dx = static_cast<float>(x) + 0.5F - center_x;
						const float dy = static_cast<float>(y) + 0.5F - center_y;
						const std::size_t index = static_cast<std::size_t>(y) * scene_width + x;
						if (dx * dx + dy * dy > radius_squared || object_depth >= frame.depth[index])
						{
							continue;
						}

						const float light = 0.76F + 0.24F * std::max(0.0F, 1.0F - std::sqrt(dx * dx + dy * dy) / radius);
						frame.depth[index] = object_depth;
						frame.color_rgba8[index * 4U + 0U] = ToByte(red * light);
						frame.color_rgba8[index * 4U + 1U] = ToByte(green * light);
						frame.color_rgba8[index * 4U + 2U] = ToByte(blue * light);
						frame.motion_rg16f[index * 2U + 0U] = FloatToHalf(-velocity_x / static_cast<float>(scene_width));
						frame.motion_rg16f[index * 2U + 1U] = FloatToHalf(-velocity_y / static_cast<float>(scene_height));
					}
				}
			};

			draw_disc(150.0F + 96.0F * time, 126.0F + 34.0F * time, 55.0F, 0.28F, 96.0F, 34.0F, 0.96F, 0.27F, 0.12F);
			draw_disc(474.0F - 72.0F * time, 236.0F - 48.0F * time, 70.0F, 0.46F, -72.0F, -48.0F, 0.12F, 0.73F, 0.94F);
			return frame;
		}
	}

	std::uint16_t FloatToHalf(float value) noexcept
	{
		const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
		const std::uint32_t sign = (bits >> 16U) & 0x8000U;
		const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
		std::uint32_t mantissa = bits & 0x7FFFFFU;

		if (exponent == 0xFFU)
		{
			return static_cast<std::uint16_t>(sign | (mantissa == 0U ? 0x7C00U : 0x7E00U));
		}

		const int half_exponent = static_cast<int>(exponent) - 127 + 15;
		if (half_exponent >= 31)
		{
			return static_cast<std::uint16_t>(sign | 0x7C00U);
		}
		if (half_exponent <= 0)
		{
			if (half_exponent < -10)
			{
				return static_cast<std::uint16_t>(sign);
			}
			mantissa = (mantissa | 0x800000U) >> static_cast<unsigned int>(1 - half_exponent);
			if ((mantissa & 0x1000U) != 0U)
			{
				mantissa += 0x2000U;
			}
			return static_cast<std::uint16_t>(sign | (mantissa >> 13U));
		}

		mantissa += 0x1000U;
		if ((mantissa & 0x800000U) != 0U)
		{
			mantissa = 0U;
			const std::uint32_t rounded_exponent = static_cast<std::uint32_t>(half_exponent + 1);
			if (rounded_exponent >= 31U)
			{
				return static_cast<std::uint16_t>(sign | 0x7C00U);
			}
			return static_cast<std::uint16_t>(sign | (rounded_exponent << 10U));
		}
		return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(half_exponent) << 10U) | (mantissa >> 13U));
	}

	float HalfToFloat(std::uint16_t value) noexcept
	{
		const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
		std::uint32_t exponent = (value >> 10U) & 0x1FU;
		std::uint32_t mantissa = value & 0x03FFU;
		std::uint32_t bits = 0U;
		if (exponent == 0U)
		{
			if (mantissa == 0U)
			{
				bits = sign;
			}
			else
			{
				int unbiased_exponent = -14;
				while ((mantissa & 0x0400U) == 0U)
				{
					mantissa <<= 1U;
					--unbiased_exponent;
				}
				mantissa &= 0x03FFU;
				bits = sign | (static_cast<std::uint32_t>(unbiased_exponent + 127) << 23U) | (mantissa << 13U);
			}
		}
		else if (exponent == 0x1FU)
		{
			bits = sign | 0x7F800000U | (mantissa << 13U);
		}
		else
		{
			exponent = exponent + 127U - 15U;
			bits = sign | (exponent << 23U) | (mantissa << 13U);
		}
		return std::bit_cast<float>(bits);
	}

	ProceduralScene GenerateProceduralScene()
	{
		return { Render(0.0F), Render(1.0F), Render(0.5F) };
	}
}
