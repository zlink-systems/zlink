/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime_failure_fixture.hpp"

#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
using namespace zlink::framework;
using detail::host::operation_completion_t;
using detail::host::pending_operation_t;
using reply_task_t = task_t<result_t<runtime::messaging::message_parts_t>>;

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

operation_completion_t successful_reply ()
{
    operation_completion_t value;
    value.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    value.parts.push_back (zlink::message_t::from (std::vector<std::uint8_t>{1, 3, 5, 7}));
    return value;
}

void verify_result (detail::mesh_node_runtime_t &runtime,
                    bool ready,
                    result_t<operation_completion_t> completion,
                    std::optional<framework_error_kind_t> expected_failure)
{
    pending_operation_t operation;
    if (ready)
        operation.completion->complete (completion);
    auto reply = runtime.await_request_reply (operation, "request cost fixture");
    if (!ready)
        operation.completion->complete (completion);
    const auto &outer = reply.result ();
    require (static_cast<bool> (outer), "reply conversion escaped as outer failure");
    const auto &result = outer.value ();
    if (expected_failure) {
        require (!result && result.error_kind () == *expected_failure,
                 "request terminal mapping changed");
    } else {
        require (static_cast<bool> (result), "successful reply failed");
        require (result.value ().items ().size () == 1, "reply part count changed");
        require (result.value ().items ()[0].to_bytes ()
                   == std::vector<std::uint8_t> ({1, 3, 5, 7}),
                 "reply payload lifetime changed");
        const auto &retained = operation.completion->task ().result ().value ();
        require (retained.parts.size () == 1 && retained.parts[0].valid (),
                 "conversion consumed the original shared result");
    }
}

// Frozen successful conversion from 6a9bd9e89f for runtime allocation comparison.
// This is a measurement reference, not a production task helper.
reply_task_t reference_reply_conversion (detail::mesh_node_runtime_t &runtime,
                                         const pending_operation_t &operation,
                                         std::string_view operation_name)
{
    using reply_result_t = result_t<runtime::messaging::message_parts_t>;
    auto source = std::make_shared<task_completion_source_t<reply_result_t>> ();
    auto output = source->task ();
    auto completion = runtime.await_completion (operation);
    detail::observe_task_terminal (
      completion, [source, operation_name] (const result_t<operation_completion_t> &result) {
          try {
              if (!result
                  || result.value ().record.terminal_result
                       != static_cast<int> (zlink::request_result_t::ok))
                  throw std::runtime_error (std::string (operation_name));
              source->complete (result_t<reply_result_t>::success (reply_result_t::success (
                runtime::messaging::message_parts_t (result.value ().parts))));
          }
          catch (...) {
              source->complete (
                detail::result_access_t::failure<reply_result_t> (std::current_exception ()));
          }
      });
    return output;
}

void verify_ready_wrapper_cost (detail::mesh_node_runtime_t &runtime)
{
    using source_t = task_completion_source_t<result_t<runtime::messaging::message_parts_t>>;
    const auto stack_source = tests::measure_allocations ([] { source_t source; });
    const auto shared_source =
      tests::measure_allocations ([] { auto source = std::make_shared<source_t> (); });
    require (shared_source.count > stack_source.count && shared_source.bytes > stack_source.bytes,
             "completion source wrapper baseline did not allocate");
    const auto wrapper_count = shared_source.count - stack_source.count;
    const auto wrapper_bytes = shared_source.bytes - stack_source.bytes;
    pending_operation_t operation;
    operation.completion->complete (
      result_t<operation_completion_t>::success (successful_reply ()));
    std::optional<reply_task_t> reference;
    std::optional<reply_task_t> actual;
    const auto before = tests::measure_allocations ([&] {
        reference.emplace (reference_reply_conversion (runtime, operation, "request cost fixture"));
    });
    const auto after = tests::measure_allocations (
      [&] { actual.emplace (runtime.await_request_reply (operation, "request cost fixture")); });
    require (reference->result ().value ().value ().items ().size () == 1
               && actual->result ().value ().value ().items ().size () == 1,
             "wrapper comparison did not execute successful runtime conversions");
    std::cout << "ready-wrapper reference-count=" << before.count << " actual-count=" << after.count
              << " wrapper-count=" << wrapper_count << " reference-bytes=" << before.bytes
              << " actual-bytes=" << after.bytes << " wrapper-bytes=" << wrapper_bytes << '\n';
    require (before.count >= wrapper_count && after.count <= before.count - wrapper_count
               && before.bytes >= wrapper_bytes && after.bytes <= before.bytes - wrapper_bytes,
             "ready request conversion retained the completion source wrapper allocation");
}

bool verify_cost (detail::mesh_node_runtime_t &runtime, bool ready)
{
    std::shared_ptr<task_completion_source_t<result_t<runtime::messaging::message_parts_t>>>
      baseline;
    const auto state_cost = tests::measure_allocations ([&] {
        baseline = std::make_shared<
          task_completion_source_t<result_t<runtime::messaging::message_parts_t>>> ();
    });
    pending_operation_t operation;
    if (ready)
        operation.completion->complete (
          result_t<operation_completion_t>::success (successful_reply ()));
    std::optional<reply_task_t> reply;
    const auto cost = tests::measure_allocations (
      [&] { reply.emplace (runtime.await_request_reply (operation, "request cost fixture")); });
    if (!ready)
        operation.completion->complete (
          result_t<operation_completion_t>::success (successful_reply ()));
    require (static_cast<bool> (reply->result ().value ()), "measured reply failed");
    std::cout << "request-reply ready=" << ready << " count=" << cost.count
              << " bytes=" << cost.bytes << " largest=" << cost.largest
              << " record=" << sizeof (operation_completion_t)
              << " state-largest=" << state_cost.largest << " state-bytes=" << state_cost.bytes
              << '\n';
    // Conversion reads the already-owned completion record; it needs no allocation
    // large enough to hold another complete infrastructure record.
    return (cost.largest <= state_cost.largest || cost.largest < sizeof (operation_completion_t))
           && cost.bytes < state_cost.bytes + sizeof (operation_completion_t);
}

void verify_completion_turn (detail::mesh_node_runtime_t &runtime)
{
    pending_operation_t operation;
    auto reply = runtime.await_request_reply (operation, "request cost fixture");
    std::promise<void> observed;
    auto done = observed.get_future ();
    detail::observe_task_completion (reply, [&] (const auto &value) {
        try {
            require (detail::current_completion_owner () != nullptr,
                     "application continuation escaped the completion turn");
            require (static_cast<bool> (value) && static_cast<bool> (value.value ()),
                     "continuation received failure");
            observed.set_value ();
        }
        catch (...) {
            observed.set_exception (std::current_exception ());
        }
    });
    detail::dispatch_completion (
      [&] {
          try {
              operation.completion->complete (
                result_t<operation_completion_t>::success (successful_reply ()));
          }
          catch (...) {
              observed.set_exception (std::current_exception ());
          }
      },
      false, &operation);
    done.get ();
}

void verify_explicit_scheduler (detail::mesh_node_runtime_t &runtime)
{
    pending_operation_t operation;
    std::function<void ()> queued;
    auto reply = detail::with_task_resume_scheduler (
      runtime.await_request_reply (operation, "request cost fixture"),
      [&] (std::function<void ()> work) { queued = std::move (work); });
    bool called = false;
    detail::observe_task_completion (reply, [&] (const auto &) { called = true; });
    std::promise<void> settled;
    auto done = settled.get_future ();
    detail::dispatch_completion (
      [&] {
          try {
              operation.completion->complete (
                result_t<operation_completion_t>::success (successful_reply ()));
              settled.set_value ();
          }
          catch (...) {
              settled.set_exception (std::current_exception ());
          }
      },
      false, &operation);
    done.get ();
    require (!called && static_cast<bool> (queued), "explicit continuation scheduler was bypassed");
    queued ();
    require (called, "explicit continuation scheduler lost callback");
}

task_t<void> consume_reply (reply_task_t reply)
{
    const auto value = co_await reply;
    value.value ();
}

void verify_owner_shutdown (detail::mesh_node_runtime_t &runtime)
{
    pending_operation_t operation;
    std::stop_source owner;
    auto consumer = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return consume_reply (runtime.await_request_reply (operation, "request owner shutdown"));
    }();
    owner.request_stop ();
    const auto &result = consumer.result ();
    require (!result && result.error_kind () == framework_error_kind_t::shutting_down,
             "owner shutdown did not finish the request consumer");
    operation.completion->complete (
      result_t<operation_completion_t>::success (successful_reply ()));
    require (!consumer.result (), "late physical reply revived the stopped request consumer");
}

void verify_failure_identity (detail::mesh_node_runtime_t &runtime, bool ready)
{
    pending_operation_t operation;
    const auto failure = std::make_exception_ptr (std::runtime_error ("request source failure"));
    if (ready)
        operation.completion->complete (
          detail::result_access_t::failure<operation_completion_t> (failure));
    auto reply = runtime.await_request_reply (operation, "request failure identity");
    if (!ready)
        operation.completion->complete (
          detail::result_access_t::failure<operation_completion_t> (failure));
    require (reply.result () && reply.result ().value ().exception () == failure,
             "request conversion replaced the original exception");
}
}

int main ()
{
    // Asynchronous completions require a live owner (#1867); it exists before any
    // allocation is measured.
    zlink::framework::completion_owner_t completion_owner;
    try {
        void *allocation = nullptr;
        const auto calibration =
          tests::measure_allocations ([&] { allocation = ::operator new (1024); });
        ::operator delete (allocation);
        require (calibration.count == 1 && calibration.bytes == 1024 && calibration.largest == 1024,
                 "allocation measurement calibration failed");
        const auto empty = tests::measure_allocations ([] {});
        require (empty.count == 0 && empty.bytes == 0 && empty.largest == 0,
                 "empty allocation measurement failed");
        auto state =
          test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("request-cost");
        detail::mesh_node_runtime_t runtime (state);
        for (const bool ready : {false, true}) {
            verify_failure_identity (runtime, ready);
            verify_result (runtime, ready,
                           result_t<operation_completion_t>::success (successful_reply ()),
                           std::nullopt);
            verify_result (runtime, ready,
                           result_t<operation_completion_t>::failure (
                             framework_error_kind_t::shutting_down, "request owner stopped"),
                           framework_error_kind_t::shutting_down);
            auto timeout = successful_reply ();
            timeout.record.terminal_result = static_cast<int> (zlink::request_result_t::timed_out);
            verify_result (runtime, ready,
                           result_t<operation_completion_t>::success (std::move (timeout)),
                           framework_error_kind_t::deadline_exceeded);
            auto terminated = successful_reply ();
            terminated.record.terminal_result =
              static_cast<int> (zlink::request_result_t::terminated);
            verify_result (runtime, ready,
                           result_t<operation_completion_t>::success (std::move (terminated)),
                           framework_error_kind_t::shutting_down);
        }
        verify_completion_turn (runtime);
        verify_explicit_scheduler (runtime);
        verify_owner_shutdown (runtime);
        std::cout << "request reply semantics PASS\n";
        verify_ready_wrapper_cost (runtime);
        const bool suspended_cost = verify_cost (runtime, false);
        const bool ready_cost = verify_cost (runtime, true);
        require (suspended_cost && ready_cost,
                 "reply conversion allocated another infrastructure record");
        std::cout << "request reply semantics and cost PASS\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
