#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace test_support
{
	enum class ImageArtifactFormat
	{
		auto_detect,
		png,
		bmp
	};

	void WriteImageArtifact(const std::filesystem::path& path,
							std::uint32_t width,
							std::uint32_t height,
							std::span<const std::uint8_t> rgba8,
							ImageArtifactFormat format = ImageArtifactFormat::auto_detect);
}
