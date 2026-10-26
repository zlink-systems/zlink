/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace zlink::framework;
namespace st = runtime::stateful;

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

// This public Store SPI is reached through the real domain repository and its
// provider error boundary; the authority factory does not receive a fake reader.
class suspended_store_t final : public location_store_t
{
  public:
    task_completion_source_t<store_read_result_t> completion;
    task_completion_source_t<void> entered;
    std::size_t reads = 0;
    task_t<store_read_result_t> read (store_key_t) override
    {
        ++reads;
        entered.complete (result_t<void>::success ());
        return completion.task ();
    }
    task_t<store_write_result_t> write (store_write_request_t) override
    {
        throw std::logic_error ("authority read unexpectedly wrote Store");
    }
    task_t<store_scan_result_t> scan (store_scan_request_t) override
    {
        throw std::logic_error ("authority read unexpectedly scanned Store");
    }
};

void verify (std::string_view terminal)
{
    require (terminal == "failed" || terminal == "cancelled" || terminal == "missing",
             "unknown provider terminal");
    suspended_store_t provider;
    runtime::provider_location_repository_t repository (provider);
    auto resolver = st::make_location_store_authority_resolver (repository);
    const st::accepted_record_authority_query_t query{.target = {st::object_kind_t::actor, "authority", 1, 1, "mesh", "target"},
       .source_node_routing_id = {'s'}, .source_node_generation = 1,
       .target_node_routing_id = {'t'}, .target_node_generation = 1,
       .requested_authority_owner_generation = 1};
    auto pending = resolver (query);
    detail::observe_task_terminal (pending, [&provider] (const auto &) {
        if (provider.reads == 0)
            provider.entered.complete (result_t<void>::failure (
              framework_error_kind_t::invalid_operation,
              "authority factory terminated before provider read"));
    });
    provider.entered.task ().result ().value ();
    require (provider.reads == 1 && !pending.await_ready (),
             ("authority factory did not suspend: reads=" + std::to_string(provider.reads)
               + " ready=" + std::to_string(pending.await_ready())).c_str ());
    if (terminal == "missing") {
        provider.completion.complete (result_t<store_read_result_t>::success (
          store_missing_t{std::chrono::system_clock::now ()}));
        require (!pending.result ().value (), "missing authority became an exceptional terminal");
        return;
    }
    const auto cause = terminal == "cancelled"
      ? detail::make_cancellation_exception ("provider read cancelled")
      : std::make_exception_ptr (std::runtime_error ("provider read failed"));
    provider.completion.complete (detail::result_access_t::failure<store_read_result_t> (cause));
    const auto result = pending.result ();
    require (!result.has_value (), "authority factory converted provider failure to nullopt");
    try {
        (void) result.value ();
        throw std::logic_error ("provider failure was lost");
    }
    catch (const framework_exception_t &error) {
        require (terminal == "failed" && error.kind () == framework_error_kind_t::unavailable,
                 "provider failure boundary changed its typed classification");
        bool nested = false;
        try { std::rethrow_if_nested (error); }
        catch (const std::runtime_error &original) {
            nested = std::string_view (original.what ()) == "provider read failed";
        }
        require (nested, "provider boundary lost the original runtime_error");
    }
    catch (const std::system_error &error) {
        require (terminal == "cancelled" && error.code () == std::errc::operation_canceled,
                 "provider cancellation lost operation_canceled");
    }
    require (provider.reads == 1, "authority failure retried provider read");
}
}

int main (int argc, char **argv)
{
    try {
        completion_owner_t completion_owner;
        struct executor_scope_t
        {
            executor_scope_t ()
            {
                runtime::configure_handler_coroutine_executor (1);
                runtime::install_host_context_hooks ();
            }
            ~executor_scope_t ()
            {
                runtime::shutdown_handler_coroutine_executor ();
                detail::set_runtime_execution_hooks (nullptr);
                detail::set_ambient_context_hooks (nullptr);
            }
        } executor;
        if (argc == 2)
            verify (argv[1]);
        else
            for (const auto terminal : {"failed", "cancelled", "missing"})
                verify (terminal);
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
