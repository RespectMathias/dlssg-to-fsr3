#pragma once

#include <cstdint>
#include <vector>

namespace test_support
{
	inline constexpr std::uint32_t scene_width = 640;
	inline constexpr std::uint32_t scene_height = 360;

	struct SceneFrame
	{
		std::uint32_t width = scene_width;
		std::uint32_t height = scene_height;
		std::vector<std::uint8_t> color_rgba8;
		std::vector<float> depth;
		std::vector<std::uint16_t> motion_rg16f;
	};

	struct ProceduralScene
	{
		SceneFrame first;
		SceneFrame second;
		SceneFrame midpoint_reference;
	};

	[[nodiscard]] std::uint16_t FloatToHalf(float value) noexcept;
	[[nodiscard]] float HalfToFloat(std::uint16_t value) noexcept;
	[[nodiscard]] ProceduralScene GenerateProceduralScene();
}
