/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <typeindex>

namespace zlink::framework
{

namespace detail
{
class timer_state_t;
class timer_runtime_t;
} // namespace detail

enum class timer_overrun_policy_t
{
    skip_late_ticks = 0,
    catch_up_bounded = 1,
    delay_next_tick = 2
};

struct timer_options_t
{
    timer_overrun_policy_t overrun_policy = timer_overrun_policy_t::skip_late_ticks;
    std::uint64_t max_catch_up_ticks = 1;
    bool stop_on_unhandled_exception = false;
};

struct timer_tick_t
{
    std::string name;
    std::uint64_t delivery_index = 0;
    std::uint64_t scheduled_index = 0;
    std::chrono::milliseconds period{0};
    std::chrono::milliseconds scheduled_elapsed{0};
    std::chrono::milliseconds started_elapsed{0};
    std::chrono::milliseconds delay{0};
    std::uint64_t skipped_ticks = 0;
};

struct timer_failure_event_t
{
    std::string timer_name;
    std::type_index handler_type;
    std::uint64_t delivery_index = 0;
    bool stopped = false;
    std::string message;
};

class timer_t
{
  public:
    timer_t ();
    ~timer_t ();

    timer_t (timer_t &&) noexcept;
    timer_t &operator= (timer_t &&) noexcept;
    timer_t (const timer_t &) = default;
    timer_t &operator= (const timer_t &) = default;

    bool is_disposed () const noexcept;
    void cancel () noexcept;

  private:
    friend class spot_context_t;
    friend class detail::timer_runtime_t;

    explicit timer_t (std::shared_ptr<detail::timer_state_t> state);

    std::shared_ptr<detail::timer_state_t> _state;
};

} // namespace zlink::framework
