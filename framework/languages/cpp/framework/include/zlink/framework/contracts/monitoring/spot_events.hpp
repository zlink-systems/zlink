/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/spots/spot.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace zlink::framework
{

namespace detail
{
class monitoring_runtime_state_t;
class monitoring_runtime_t;
class app_state_t;
}

enum class spot_event_kind_t
{
    timer_handler_failed = 0,
    timer_stopped_after_unhandled_exception = 1
};

struct spot_timer_diagnostic_t
{
    spot_id_t spot_id;
    std::string timer_name;
    std::string handler_type;
    std::uint64_t delivery_index = 0;
    std::string message;

    friend bool operator== (const spot_timer_diagnostic_t &,
                            const spot_timer_diagnostic_t &) = default;
};

struct spot_event_t
{
    std::string source_name;
    std::chrono::system_clock::time_point timestamp{};
    spot_event_kind_t event = spot_event_kind_t::timer_handler_failed;
    spot_timer_diagnostic_t diagnostic;

    friend bool operator== (const spot_event_t &,
                            const spot_event_t &) = default;
};

using spot_event_handler_t = std::function<void (const spot_event_t &)>;

class monitoring_builder_t
{
  public:
    monitoring_builder_t ();
    ~monitoring_builder_t ();

    monitoring_builder_t (monitoring_builder_t &&) noexcept;
    monitoring_builder_t &operator= (monitoring_builder_t &&) noexcept;
    monitoring_builder_t (const monitoring_builder_t &) = delete;
    monitoring_builder_t &operator= (const monitoring_builder_t &) = delete;

    monitoring_builder_t &add_spot_events (std::string source_name);
    monitoring_builder_t &on_spot_event (spot_event_handler_t handler);

  private:
    friend class app_t;
    friend class detail::app_state_t;
    friend class detail::monitoring_runtime_t;
    explicit monitoring_builder_t (
      std::shared_ptr<detail::monitoring_runtime_state_t> state);

    std::shared_ptr<detail::monitoring_runtime_state_t> _state;
};

} // namespace zlink::framework
