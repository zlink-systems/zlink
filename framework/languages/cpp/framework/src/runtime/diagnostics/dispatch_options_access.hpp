/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"

#include <functional>
#include <optional>
#include <utility>

namespace zlink::framework::detail
{

class dispatch_options_access_t
{
  public:
    static const std::optional<logger_t<>> &logger (
      const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_logger;
    }

    static void set_logger (
      dispatch_options_t &options,
      std::optional<logger_t<>> logger)
    {
        options._diagnostics_logger = std::move (logger);
    }

    static void set_live_mode (
      dispatch_options_t &options,
      std::shared_ptr<std::atomic<message_flow_log_mode_t>> live)
    {
        options._live_mode = std::move (live);
        if (!options._diagnostics_source_generation
            || options._diagnostics_source_generation->load (std::memory_order_relaxed) == 0) {
            static std::atomic<std::uint64_t> next_generation{0};
            options._diagnostics_source_generation =
              std::make_shared<std::atomic<std::uint64_t>> (
                next_generation.fetch_add (1, std::memory_order_relaxed) + 1);
        }
        if (!options._diagnostics_local_sequence) {
            options._diagnostics_local_sequence =
              std::make_shared<std::atomic<std::uint64_t>> (0);
        }
    }

    static std::uint64_t source_mesh_generation (
      const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_source_generation
          ? options._diagnostics_source_generation->load (std::memory_order_relaxed)
          : 0;
    }

    static std::uint64_t next_local_sampling_sequence (
      const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_local_sequence
          ? options._diagnostics_local_sequence->fetch_add (1, std::memory_order_relaxed) + 1
          : 1;
    }

    static void set_source_mesh_generation (
      dispatch_options_t &options, std::uint64_t generation) noexcept
    {
        if (options._diagnostics_source_generation) {
            options._diagnostics_source_generation->store (generation,
                                                            std::memory_order_relaxed);
        }
    }

    static message_flow_log_mode_t effective_message_flow (
      const dispatch_options_t &options) noexcept
    {
        return options._live_mode
          ? options._live_mode->load (std::memory_order_relaxed)
          : options.diagnostics.message_flow ();
    }

    static void set_observer_for_tests (
      dispatch_options_t &options,
      std::function<void (const message_flow_event_t &)> observer)
    {
        options._message_flow_observer =
          [observer = std::move (observer)] (const void *event) {
              observer (*static_cast<const message_flow_event_t *> (event));
          };
    }

    static void observe (
      const dispatch_options_t &options,
      const message_flow_event_t &event)
    {
        if (options._message_flow_observer)
            options._message_flow_observer (&event);
    }

    static bool has_observer (const dispatch_options_t &options) noexcept
    {
        return static_cast<bool> (options._message_flow_observer);
    }

    static void set_dispatch_error_observer_for_tests (
      dispatch_options_t &options,
      std::function<void (const message_dispatch_error_event_t &)> observer)
    {
        options._dispatch_error_observer =
          [observer = std::move (observer)] (const void *event) {
              observer (
                *static_cast<const message_dispatch_error_event_t *> (event));
          };
    }

    static void observe_dispatch_error (
      const dispatch_options_t &options,
      const message_dispatch_error_event_t &event)
    {
        if (options._dispatch_error_observer)
            options._dispatch_error_observer (&event);
    }

    static bool has_dispatch_error_observer (
      const dispatch_options_t &options) noexcept
    {
        return static_cast<bool> (options._dispatch_error_observer);
    }
};

} // namespace zlink::framework::detail
