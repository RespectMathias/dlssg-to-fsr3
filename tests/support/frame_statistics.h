#pragma once

#include <chrono>
#include <cstddef>
#include <deque>

namespace test_support
{
	struct FrameStatisticsSnapshot
	{
		std::size_t sample_count = 0U;
		double fps = 0.0;
		double average_ms = 0.0;
		double minimum_ms = 0.0;
		double maximum_ms = 0.0;
		double p50_ms = 0.0;
		double p95_ms = 0.0;
		double p99_ms = 0.0;
	};

	class RollingFrameStatistics
	{
	public:
		explicit RollingFrameStatistics(std::size_t capacity = 120U);

		void Add(std::chrono::duration<double, std::milli> frame_time);
		void AddMilliseconds(double frame_time_ms);
		void Clear() noexcept;
		[[nodiscard]] std::size_t Capacity() const noexcept;
		[[nodiscard]] std::size_t Size() const noexcept;
		[[nodiscard]] FrameStatisticsSnapshot Snapshot() const;

	private:
		std::size_t capacity_;
		std::deque<double> samples_ms_;
	};
}
