/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/handlers/handler_registry_runtime.hpp"
#include <atomic>
#include <functional>
#include <utility>

namespace zlink::framework::detail
{
namespace
{
using reply_t = std::optional<route_dispatch_reply_t>;
using invocation_t = route_handler_invoker_t;

// These are the former coroutine locals. A suspended invocation owns its storage;
// an inline invocation reuses the calling thread's unborrowed storage.
struct invocation_storage_t
{
    std::optional<runtime::application_dispatch_terminal_owner_t> terminal;
    zlink::message_t message;
    std::optional<framework::route_message_context_t> context;
    std::function<void ()> before;
    invocation_t::completion_t complete;
    std::optional<service_scope_t> scope;
    const route_handler_registry_t::entry_t *handler = nullptr;
    serializer_registry_t *serializers = nullptr;
    handler_dispatch_kind_t kind;

    struct entry_callback_t
    {
        invocation_storage_t *storage;
        void operator() () const
        {
            storage->terminal->enter_handler ();
            if (storage->before)
                storage->before ();
            if (const auto *completion = std::get_if<route_completion_t> (&storage->complete))
                completion->entered (*storage->context, storage->kind);
        }
    } entry{this};

    static task_t<zlink::message_t> start (const std::shared_ptr<void> &opaque)
    {
        auto &storage = *static_cast<invocation_storage_t *> (opaque.get ());
        return storage.handler->invoker (storage.scope->provider (), *storage.serializers,
                                         storage.message, *storage.context,
                                         handler_start_callback_ref_t (storage.entry, opaque));
    }
};

result_t<reply_t> finish_invocation (invocation_storage_t &storage,
                                     const result_t<zlink::message_t> &raw,
                                     std::optional<task_t<zlink::message_t>> &pending)
{
    std::optional<result_t<zlink::message_t>> failure;
    const auto *result = &raw;
    if (!raw) {
        try {
            (void) raw.value ();
        }
        catch (const framework_exception_t &error) {
            failure.emplace (result_access_t::failure<zlink::message_t> (error));
        }
        catch (...) {
            failure.emplace (
              is_request_dispatch (storage.kind)
                ? result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                       "routed request handler threw an exception")
                : current_exception_result<zlink::message_t> ());
        }
        result = &*failure;
    }
    auto completed = result_t<reply_t>::success (std::nullopt);
    try {
        completed = std::visit (
          [&] (const auto &completion) {
              using completion_type = std::decay_t<decltype (completion)>;
              if constexpr (std::is_same_v<completion_type, route_completion_t>)
                  return completion (*result, *storage.context, storage.kind);
              else if constexpr (std::is_same_v<completion_type,
                                                invocation_t::callback_completion_t>) {
                  if (completion)
                      return completion (*result);
              }
              return *result ? result_t<reply_t>::success (std::nullopt)
                             : propagate_failure<reply_t> (*result, "routed handler failed");
          },
          storage.complete);
    }
    catch (...) {
        completed = current_exception_result<reply_t> ();
    }
    if (storage.scope)
        storage.scope->close ();
    storage.scope.reset ();
    storage.before = {};
    storage.message = {};
    pending.reset ();
    storage.complete = {};
    storage.context.reset ();
    storage.terminal.reset ();
    return completed;
}

// Await readiness without copying a successful native message out of its task.
struct pending_result_t
{
    task_t<zlink::message_t> &task;
    bool await_ready () const noexcept { return task.await_ready (); }
    template <typename TPromise> void await_suspend (std::coroutine_handle<TPromise> next)
    {
        task.await_suspend (next);
    }
    void await_resume ()
    {
        if (const auto failure = take_serial_resume_failure ())
            throw framework_exception_t (failure->kind, failure->message);
    }
};

task_t<reply_t> finish_pending_invocation (std::shared_ptr<invocation_storage_t> storage,
                                           std::optional<task_t<zlink::message_t>> pending)
{
    std::optional<result_t<zlink::message_t>> failure;
    try {
        co_await pending_result_t{*pending};
    }
    catch (...) {
        failure.emplace (current_exception_result<zlink::message_t> ());
    }
    const auto &result = failure ? *failure : task_result_access_t::read (*pending);
    co_return finish_invocation (*storage, result, pending);
}
} // namespace

task_t<std::optional<route_dispatch_reply_t>>
route_handler_invoker_t::invoke (const route_handler_registry_t::entry_t &handler,
                                 const handler_registry_t &filters,
                                 handler_dispatch_kind_t dispatch_kind,
                                 service_provider_t &services,
                                 serializer_registry_t &serializers,
                                 zlink::message_t message,
                                 framework::route_message_context_t context,
                                 std::function<void ()> before,
                                 completion_t complete,
                                 runtime::application_dispatch_terminal_owner_t terminal,
                                 runtime::offload_executor_t *executor)
{
    auto storage = runtime::acquire_dispatch_storage<invocation_storage_t> ();
    storage->terminal.emplace (std::move (terminal));
    storage->message = std::move (message);
    storage->context.emplace (std::move (context));
    storage->before = std::move (before);
    storage->complete = std::move (complete);
    storage->handler = &handler;
    storage->serializers = &serializers;
    storage->kind = dispatch_kind;
    std::optional<task_t<zlink::message_t>> pending;
    std::optional<result_t<zlink::message_t>> failure;
    try {
        storage->scope.emplace (
          service_scope_t::create (services, service_scope_kind_t::handler_invocation));
        pending.emplace (filters.invoke_filters_async (
          dispatch_kind, storage->scope->provider (), serializers, *storage->context,
          prepared_filter_terminal_t{storage, &invocation_storage_t::start}));
    }
    catch (...) {
        failure.emplace (current_exception_result<zlink::message_t> ());
    }
    if (failure)
        return task_t<reply_t> (finish_invocation (*storage, *failure, pending));
    if (pending->await_ready ())
        return task_t<reply_t> (
          finish_invocation (*storage, task_result_access_t::read (*pending), pending));
    // Once work escapes this call, only its physical continuation owns the state.
    // In particular, failed continuation-frame allocation must not leave a live
    // scope/terminal in the thread's reusable inline slot.
    auto &cached = runtime::dispatch_storage<invocation_storage_t> ();
    if (cached == storage)
        cached.reset ();
    if (executor)
        storage->terminal->retain_physical_work (executor->retain_internal_work ());
    return finish_pending_invocation (std::move (storage), std::move (pending));
}
} // namespace zlink::framework::detail
