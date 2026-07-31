#include "frame_statistics.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace test_support
{
	namespace
	{
		double Percentile(const std::vector<double>& sorted, double percentile)
		{
			const double position = percentile * static_cast<double>(sorted.size() - 1U);
			const std::size_t lower = static_cast<std::size_t>(position);
			const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
			const double fraction = position - static_cast<double>(lower);
			return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
		}
	}

	RollingFrameStatistics::RollingFrameStatistics(std::size_t capacity) : capacity_(capacity)
	{
		if (capacity == 0U)
		{
			throw std::invalid_argument("Rolling frame statistics capacity must be non-zero");
		}
	}

	void RollingFrameStatistics::Add(std::chrono::duration<double, std::milli> frame_time)
	{
		AddMilliseconds(frame_time.count());
	}

	void RollingFrameStatistics::AddMilliseconds(double frame_time_ms)
	{
		if (!std::isfinite(frame_time_ms) || frame_time_ms <= 0.0)
		{
			throw std::invalid_argument("Frame time must be finite and positive");
		}
		samples_ms_.push_back(frame_time_ms);
		if (samples_ms_.size() > capacity_)
		{
			samples_ms_.pop_front();
		}
	}

	void RollingFrameStatistics::Clear() noexcept
	{
		samples_ms_.clear();
	}

	std::size_t RollingFrameStatistics::Capacity() const noexcept
	{
		return capacity_;
	}

	std::size_t RollingFrameStatistics::Size() const noexcept
	{
		return samples_ms_.size();
	}

	FrameStatisticsSnapshot RollingFrameStatistics::Snapshot() const
	{
		FrameStatisticsSnapshot result;
		result.sample_count = samples_ms_.size();
		if (samples_ms_.empty())
		{
			return result;
		}

		std::vector<double> sorted(samples_ms_.begin(), samples_ms_.end());
		std::sort(sorted.begin(), sorted.end());
		result.average_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
		result.fps = 1000.0 / result.average_ms;
		result.minimum_ms = sorted.front();
		result.maximum_ms = sorted.back();
		result.p50_ms = Percentile(sorted, 0.50);
		result.p95_ms = Percentile(sorted, 0.95);
		result.p99_ms = Percentile(sorted, 0.99);
		return result;
	}
}
