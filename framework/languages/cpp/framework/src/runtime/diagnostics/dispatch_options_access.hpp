/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"
#include "runtime/diagnostics/process_telemetry_delivery.hpp"

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace zlink::framework::detail
{

class dispatch_options_access_t
{
  public:
    static const std::optional<logger_t<>> &logger (const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_logger;
    }

    static void set_logger (dispatch_options_t &options, std::optional<logger_t<>> logger)
    {
        options._diagnostics_logger = std::move (logger);
        ensure_registration (options);
    }

    static void set_live_mode (dispatch_options_t &options,
                               std::shared_ptr<std::atomic<message_flow_log_mode_t>> live)
    {
        options._live_mode = std::move (live);
        if (!options._diagnostics_source_generation
            || options._diagnostics_source_generation->load (std::memory_order_relaxed) == 0) {
            static std::atomic<std::uint64_t> next_generation{0};
            options._diagnostics_source_generation = std::make_shared<std::atomic<std::uint64_t>> (
              next_generation.fetch_add (1, std::memory_order_relaxed) + 1);
        }
        if (!options._diagnostics_local_sequence) {
            options._diagnostics_local_sequence = std::make_shared<std::atomic<std::uint64_t>> (0);
        }
    }

    static std::uint64_t source_mesh_generation (const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_source_generation
                 ? options._diagnostics_source_generation->load (std::memory_order_relaxed)
                 : 0;
    }

    static std::uint64_t next_local_sampling_sequence (const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_local_sequence
                 ? options._diagnostics_local_sequence->fetch_add (1, std::memory_order_relaxed) + 1
                 : 1;
    }

    static void set_source_mesh_generation (dispatch_options_t &options,
                                            std::uint64_t generation) noexcept
    {
        if (options._diagnostics_source_generation) {
            options._diagnostics_source_generation->store (generation, std::memory_order_relaxed);
        }
    }

    static message_flow_log_mode_t
    effective_message_flow (const dispatch_options_t &options) noexcept
    {
        return options._live_mode ? options._live_mode->load (std::memory_order_relaxed)
                                  : options.diagnostics.message_flow ();
    }

    static void set_observer_for_tests (dispatch_options_t &options,
                                        std::function<void (const message_flow_event_t &)> observer)
    {
        options._message_flow_observer = {};
        if (observer) {
            options._message_flow_observer = [observer = std::move (observer)] (const void *event) {
                observer (*static_cast<const message_flow_event_t *> (event));
            };
        }
        ensure_registration (options);
    }

    static bool has_observer (const dispatch_options_t &options) noexcept
    {
        return static_cast<bool> (options._message_flow_observer);
    }

    static void set_dispatch_error_observer_for_tests (
      dispatch_options_t &options,
      std::function<void (const message_dispatch_error_event_t &)> observer)
    {
        options._dispatch_error_observer = {};
        if (observer) {
            options._dispatch_error_observer = [observer = std::move (observer)] (const void *event) {
                observer (*static_cast<const message_dispatch_error_event_t *> (event));
            };
        }
        ensure_registration (options);
    }

    static bool has_dispatch_error_observer (const dispatch_options_t &options) noexcept
    {
        return static_cast<bool> (options._dispatch_error_observer);
    }
    static void bind_telemetry (dispatch_options_t &options,
                                runtime::metric_provider_t provider,
                                opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter)
    {
        ensure_registration (options);
        if (options._telemetry_registration)
            bind_process_telemetry_metrics (options._telemetry_registration->id,
                                            std::move (provider), std::move (meter));
    }

    static void retire_telemetry (const dispatch_options_t &options) noexcept
    {
        if (options._telemetry_registration)
            retire_process_telemetry (options._telemetry_registration->id);
    }

    template <typename Event>
    static void submit (const dispatch_options_t &options, Event event,
                        message_flow_log_mode_t mode) noexcept
    {
        try {
            telemetry_record_t record;
            record.registration = options._telemetry_registration->id;
            record.mode = mode;
            record.include_sizes = options.diagnostics.include_message_sizes ();
            if constexpr (std::is_same_v<Event, message_flow_event_t>)
                record.observer = options._message_flow_observer;
            else
                record.observer = options._dispatch_error_observer;
            if (event.exception) {
                auto summary = diagnostic_event_sink_t::exception_summary (event.exception);
                if constexpr (std::is_same_v<Event, message_dispatch_error_event_t>) {
                    event.error_type = std::move (summary.type);
                    event.error_message = std::move (summary.message);
                } else {
                    record.error = std::move (summary);
                }
            }
            if (!record.observer)
                event.exception = nullptr;
            record.event = std::move (event);
            record.logger = options._diagnostics_logger;
            submit_process_telemetry (std::move (record));
        }
        catch (...) {
            telemetry_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

  private:
    static void ensure_registration (dispatch_options_t &options)
    {
        if (!options._telemetry_registration
            && process_telemetry_has_provider (options, telemetry_event_kind_t::any))
            options._telemetry_registration = register_process_telemetry ();
    }

};

} // namespace zlink::framework::detail
