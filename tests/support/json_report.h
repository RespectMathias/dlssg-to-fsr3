#pragma once

#include "frame_statistics.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace test_support
{
	struct JsonReport
	{
		std::string name;
		bool passed = false;
		std::map<std::string, std::string> metadata;
		std::map<std::string, double> metrics;
		std::optional<FrameStatisticsSnapshot> frame_statistics;
		std::vector<std::filesystem::path> artifacts;
	};

	void WriteJsonReport(const std::filesystem::path& path, const JsonReport& report);
}
