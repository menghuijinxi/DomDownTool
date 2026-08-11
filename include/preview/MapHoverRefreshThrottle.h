#pragma once

#include <chrono>
#include <optional>

namespace DomDownToolPreview
{
class MapHoverRefreshThrottle
{
  public:
    using Clock = std::chrono::steady_clock;

    explicit MapHoverRefreshThrottle(Clock::duration interval)
        : interval_(interval < Clock::duration::zero() ? Clock::duration::zero() : interval)
    {
    }

    [[nodiscard]] bool shouldRefresh(Clock::time_point now)
    {
        if (lastRefresh_.has_value() && now - *lastRefresh_ < interval_)
        {
            return false;
        }
        lastRefresh_ = now;
        return true;
    }

    void reset()
    {
        lastRefresh_.reset();
    }

  private:
    Clock::duration interval_;
    std::optional<Clock::time_point> lastRefresh_;
};
} // namespace DomDownToolPreview
