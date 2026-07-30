#include "json_report.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <string_view>

namespace test_support
{
	namespace
	{
		void WriteEscaped(std::ostream& output, std::string_view value)
		{
			static constexpr char hex[] = "0123456789abcdef";
			output.put('"');
			for (const unsigned char character : value)
			{
				switch (character)
				{
				case '"':
					output << "\\\"";
					break;
				case '\\':
					output << "\\\\";
					break;
				case '\b':
					output << "\\b";
					break;
				case '\f':
					output << "\\f";
					break;
				case '\n':
					output << "\\n";
					break;
				case '\r':
					output << "\\r";
					break;
				case '\t':
					output << "\\t";
					break;
				default:
					if (character < 0x20U)
					{
						output << "\\u00" << hex[character >> 4U] << hex[character & 0x0FU];
					}
					else
					{
						output.put(static_cast<char>(character));
					}
					break;
				}
			}
			output.put('"');
		}

		void WriteNumber(std::ostream& output, double value)
		{
			if (std::isfinite(value))
			{
				output << value;
			}
			else
			{
				output << "null";
			}
		}

		std::string PathToUtf8(const std::filesystem::path& path)
		{
			const std::u8string value = path.generic_u8string();
			std::string result;
			result.reserve(value.size());
			for (const char8_t character : value)
			{
				result.push_back(static_cast<char>(character));
			}
			return result;
		}
	}

	void WriteJsonReport(const std::filesystem::path& path, const JsonReport& report)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			throw std::runtime_error("Could not open JSON report for writing");
		}
		output.imbue(std::locale::classic());
		output << std::setprecision(17);
		output << "{\n  \"name\": ";
		WriteEscaped(output, report.name);
		output << ",\n  \"passed\": " << (report.passed ? "true" : "false");

		output << ",\n  \"metadata\": {";
		bool first = true;
		for (const auto& [key, value] : report.metadata)
		{
			output << (first ? "\n" : ",\n") << "    ";
			WriteEscaped(output, key);
			output << ": ";
			WriteEscaped(output, value);
			first = false;
		}
		output << (first ? "}" : "\n  }");

		output << ",\n  \"metrics\": {";
		first = true;
		for (const auto& [key, value] : report.metrics)
		{
			output << (first ? "\n" : ",\n") << "    ";
			WriteEscaped(output, key);
			output << ": ";
			WriteNumber(output, value);
			first = false;
		}
		output << (first ? "}" : "\n  }");

		if (report.frame_statistics.has_value())
		{
			const FrameStatisticsSnapshot& statistics = *report.frame_statistics;
			output << ",\n  \"frame_statistics\": {\n"
				   << "    \"sample_count\": " << statistics.sample_count << ",\n"
				   << "    \"fps\": ";
			WriteNumber(output, statistics.fps);
			output << ",\n    \"average_ms\": ";
			WriteNumber(output, statistics.average_ms);
			output << ",\n    \"minimum_ms\": ";
			WriteNumber(output, statistics.minimum_ms);
			output << ",\n    \"maximum_ms\": ";
			WriteNumber(output, statistics.maximum_ms);
			output << ",\n    \"p50_ms\": ";
			WriteNumber(output, statistics.p50_ms);
			output << ",\n    \"p95_ms\": ";
			WriteNumber(output, statistics.p95_ms);
			output << ",\n    \"p99_ms\": ";
			WriteNumber(output, statistics.p99_ms);
			output << "\n  }";
		}

		output << ",\n  \"artifacts\": [";
		for (std::size_t index = 0; index < report.artifacts.size(); ++index)
		{
			output << (index == 0U ? "\n    " : ",\n    ");
			WriteEscaped(output, PathToUtf8(report.artifacts[index]));
		}
		output << (report.artifacts.empty() ? "]\n}\n" : "\n  ]\n}\n");
		if (!output)
		{
			throw std::runtime_error("Failed while writing JSON report");
		}
	}
}
