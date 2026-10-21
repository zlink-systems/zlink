/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/execution/actor_execution_context.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{
using namespace zlink::framework;
using runtime::flow_context_t;
using runtime::flow_value_t;

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

flow_value_t flow (char value)
{
    return {std::string (36, value), flow_origin_t::application, message_flow_log_mode_t::normal,
            std::string (512, value)};
}

void verify_scopes ()
{
    static_assert (std::is_nothrow_move_constructible_v<std::optional<flow_value_t>>);
    static_assert (std::is_nothrow_move_assignable_v<std::optional<flow_value_t>>);
    require (!flow_context_t::current (), "scope test inherited a flow");
    {
        flow_context_t::scope_t outer (flow ('a'));
        {
            flow_context_t::scope_t nested (flow ('b'));
            require (flow_context_t::current ()->flow_id == std::string (36, 'b'),
                     "nested flow was not installed");
        }
        require (flow_context_t::current ()->stream_session_id == std::string (512, 'a'),
                 "nested scope did not restore the previous session");
        try {
            flow_context_t::scope_t nested (flow ('c'));
            throw 1;
        }
        catch (int) {
        }
        require (flow_context_t::current ()->flow_id == std::string (36, 'a'),
                 "exception did not restore the previous flow");
        {
            auto off = flow_context_t::enter_current_or_create (flow_origin_t::application,
                                                                message_flow_log_mode_t::off);
            require (!flow_context_t::current (), "Off did not suppress the previous flow");
        }
        require (flow_context_t::current ()->stream_session_id == std::string (512, 'a'),
                 "Off scope did not restore the previous flow");
    }
    require (!flow_context_t::current (), "scope leaked its flow");
}

void verify_combined_ambient ()
{
    const auto *previous_hooks = detail::current_ambient_context_hooks ();
    struct restore_hooks_t
    {
        const detail::ambient_context_hooks_t *previous;
        ~restore_hooks_t () { detail::set_ambient_context_hooks (previous); }
    } restore{previous_hooks};
    detail::set_ambient_context_hooks (&runtime::ambient_flow_hooks ());
    detail::ambient_context_snapshot_t captured;
    {
        flow_context_t::scope_t outer (flow ('a'));
        runtime::actor_execution_scope_t actor ("captured-actor", "captured-spot");
        detail::stream_header_t header (detail::stream_message_kind_t::request,
                                        stream_codec_t::json, detail::stream_header_flags_t::none,
                                        std::nullopt, "captured-relay");
        header.with_correlation_id ("captured-correlation");
        const detail::stream_relay_dispatch_scope_t relay (std::move (header));
        captured = detail::capture_ambient_context ();
    }
    {
        flow_context_t::scope_t outer (flow ('b'));
        runtime::actor_execution_scope_t actor ("previous-actor", "previous-spot");
        detail::stream_header_t header (detail::stream_message_kind_t::request,
                                        stream_codec_t::json, detail::stream_header_flags_t::none,
                                        std::nullopt, "previous-relay");
        const detail::stream_relay_dispatch_scope_t relay (std::move (header));
        try {
            const auto entered = detail::enter_ambient_context (captured);
            require (flow_context_t::current ()->stream_session_id == std::string (512, 'a'),
                     "captured flow session was not installed");
            require (runtime::current_actor_execution.actor_key == "captured-actor",
                     "captured actor was not installed");
            require (detail::current_stream_relay_dispatch ()->packet_name () == "captured-relay",
                     "captured relay was not installed");
            throw 1;
        }
        catch (int) {
        }
        require (flow_context_t::current ()->flow_id == std::string (36, 'b'),
                 "ambient entry did not restore previous flow");
        require (runtime::current_actor_execution.actor_key == "previous-actor",
                 "ambient entry did not restore previous actor");
        require (detail::current_stream_relay_dispatch ()->packet_name () == "previous-relay",
                 "ambient entry did not restore previous relay");
    }
    require (!flow_context_t::current () && runtime::current_actor_execution.actor_key.empty ()
               && !detail::current_stream_relay_dispatch (),
             "ambient entry leaked a context");
}

void verify_previous_value_cost ()
{
    flow_context_t::scope_t outer (flow ('a'));
    const auto copy = tests::measure_allocations ([] {
        auto copied = flow_context_t::current ();
        require (copied->stream_session_id->size () == 512, "copy baseline lost its session");
    });
    require (copy.count != 0 && copy.bytes != 0, "copy baseline did not allocate");
    const auto actual = tests::measure_allocations ([] {
        flow_context_t::scope_t suppress (std::nullopt);
        require (!flow_context_t::current (), "measured scope did not suppress its flow");
    });
    require (flow_context_t::current ()->stream_session_id == std::string (512, 'a'),
             "measured scope lost previous flow storage");
    std::cout << "ambient-previous copy-count=" << copy.count << " copy-bytes=" << copy.bytes
              << " actual-count=" << actual.count << " actual-bytes=" << actual.bytes << '\n';
    require (actual.count == 0 && actual.bytes == 0,
             "ambient scope copied the previous flow instead of transferring its storage");
}
} // namespace

int main ()
{
    // Asynchronous completions require a live owner (#1867); it exists before any
    // allocation is measured.
    zlink::framework::completion_owner_t completion_owner;
    try {
        verify_scopes ();
        verify_combined_ambient ();
        verify_previous_value_cost ();
        std::cout << "ambient scope semantics and cost PASS\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
