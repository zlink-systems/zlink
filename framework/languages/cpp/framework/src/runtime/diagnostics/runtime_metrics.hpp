/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "runtime/diagnostics/monitoring_runtime.hpp"

#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>

namespace zlink::framework::runtime
{

/* The application installs its standard MeterProvider before creating the host.
 * Metric collection is independent of logging and message-flow diagnostics. */
class runtime_metrics_t
{
  public:
    explicit runtime_metrics_t (
      std::shared_ptr<framework::detail::monitoring_runtime_state_t> state) :
        _state (std::move (state))
    {
    }

    bool enabled () const noexcept { return _state && _state->metric_meter != nullptr; }

    void counter (std::string name,
                  std::string unit,
                  double delta,
                  std::map<std::string, std::string> tags = {}) const
    {
        emit (std::move (name), std::move (unit), delta,
              framework::detail::metric_instrument_kind_t::counter,
              framework::detail::metric_temporality_t::delta, std::move (tags));
    }

    void updown (std::string name,
                 std::string unit,
                 double delta,
                 std::map<std::string, std::string> tags = {}) const
    {
        emit (std::move (name), std::move (unit), delta,
              framework::detail::metric_instrument_kind_t::updown,
              framework::detail::metric_temporality_t::delta, std::move (tags));
    }

    void observable (std::string name,
                     std::string unit,
                     double current,
                     std::map<std::string, std::string> tags = {}) const
    {
        emit (std::move (name), std::move (unit), current,
              framework::detail::metric_instrument_kind_t::observable,
              framework::detail::metric_temporality_t::current, std::move (tags));
    }

    void histogram (std::string name,
                    std::string unit,
                    double sample,
                    std::map<std::string, std::string> tags = {}) const
    {
        emit (std::move (name), std::move (unit), sample,
              framework::detail::metric_instrument_kind_t::histogram,
              framework::detail::metric_temporality_t::sample, std::move (tags));
    }

  private:
    void emit (std::string name,
               std::string unit,
               double value,
               framework::detail::metric_instrument_kind_t instrument_kind,
               framework::detail::metric_temporality_t temporality,
               std::map<std::string, std::string> tags) const
    {
        if (!enabled ()) {
            return;
        }
        framework::detail::monitoring_runtime_t (_state).publish_metric (
          framework::detail::metric_event_payload_t{
            std::move (name), value, std::move (unit), instrument_kind, temporality,
            std::move (tags)});
    }

    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _state;
};

} // namespace zlink::framework::runtime
