/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include <fstream>
#include <condition_variable>
#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/opaque_store_record_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

class deferred_relocation_provider_t final : public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        return inner.put (std::move (reference), payload, retention);
    }
    task_t<blob_read_result_t> read (blob_reference_t) override
    {
        entered.set_value ();
        return read_completion.task ();
    }
    task_t<blob_renew_result_t> renew (blob_reference_t, std::chrono::milliseconds) override
    {
        entered.set_value ();
        return renew_completion.task ();
    }
    task_t<void> erase (blob_reference_t) override
    {
        entered.set_value ();
        return erase_completion.task ();
    }
    in_memory_relocation_store_t inner;
    std::promise<void> entered;
    task_completion_source_t<blob_read_result_t> read_completion;
    task_completion_source_t<blob_renew_result_t> renew_completion;
    task_completion_source_t<void> erase_completion;
};

TEST (CppFrameworkOpaqueRelocationStore, ProviderTasksYieldHandlerWorkerUntilCompletion)
{
    configure_handler_coroutine_executor (1);
    install_host_context_hooks ();
    for (int operation = 0; operation < 3; ++operation) {
        SCOPED_TRACE (operation);
        deferred_relocation_provider_t provider;
        provider_relocation_repository_t repository (provider);
        auto entered = provider.entered.get_future ();
        std::promise<void> probe;
        auto probed = probe.get_future ();
        std::promise<bool> completion;
        auto completed = completion.get_future ();
        const auto observe = [&completion] (auto pending) {
            detail::observe_task_completion (pending, [&completion] (const auto &result) {
                completion.set_value (result.has_value ());
            });
        };
        handler_coroutine_executor ().post_native_continuation ([&] {
            if (operation == 0)
                observe (repository.get_relocation ("held"));
            else if (operation == 1)
                observe (repository.renew_relocation ("held", std::chrono::hours (1)));
            else
                observe (repository.delete_relocation ("held"));
        });
        entered.wait ();
        handler_coroutine_executor ().post_native_continuation ([&probe] { probe.set_value (); });
        const bool yielded = probed.wait_for (300ms) == std::future_status::ready;
        provider.read_completion.complete (
          result_t<blob_read_result_t>::success (blob_missing_t{}));
        provider.renew_completion.complete (
          result_t<blob_renew_result_t>::success (blob_missing_t{}));
        provider.erase_completion.complete (result_t<void>::success ());
        EXPECT_TRUE (completed.get ());
        probed.wait ();
        EXPECT_TRUE (yielded);
    }
    shutdown_handler_coroutine_executor ();
}

class post_commit_failure_relocation_store_t final : public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        auto committed = co_await inner.put (reference, payload, retention);
        if (_fail_next_put) {
            _fail_next_put = false;
            co_return result_t<blob_put_result_t>::failure (framework_error_kind_t::unavailable,
                                                            "reply was lost after commit");
        }
        co_return committed;
    }

    task_t<blob_read_result_t> read (blob_reference_t reference) override
    {
        return inner.read (std::move (reference));
    }

    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override
    {
        return inner.renew (std::move (reference), retention);
    }

    task_t<void> erase (blob_reference_t reference) override
    {
        return inner.erase (std::move (reference));
    }

    in_memory_relocation_store_t inner;

  private:
    bool _fail_next_put = true;
};

class uncertain_put_relocation_store_t final : public relocation_store_t
{
  public:
    explicit uncertain_put_relocation_store_t (
      framework_error_kind_t error_kind = framework_error_kind_t::unavailable) :
        _error_kind (error_kind)
    {
    }

    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        put_references.push_back (reference.value);
        if (put_references.size () < 3)
            return task_t<blob_put_result_t> (
              result_t<blob_put_result_t>::failure (_error_kind, "put response uncertain"));
        return inner.put (std::move (reference), payload, retention);
    }
    task_t<blob_read_result_t> read (blob_reference_t reference) override
    {
        read_references.push_back (reference.value);
        return inner.read (std::move (reference));
    }
    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override
    {
        return inner.renew (std::move (reference), retention);
    }
    task_t<void> erase (blob_reference_t reference) override
    {
        return inner.erase (std::move (reference));
    }
    in_memory_relocation_store_t inner;
    std::vector<std::string> put_references;
    std::vector<std::string> read_references;

  private:
    framework_error_kind_t _error_kind;
};

TEST (CppFrameworkOpaqueRelocationStore, MissingReadBackRetriesSameReferenceAfterProviderError)
{
    uncertain_put_relocation_store_t provider (framework_error_kind_t::protocol_error);
    provider_relocation_repository_t repository (provider);
    const auto result =
      repository
        .put_relocation (bytes ("uncommitted"), 1h, std::chrono::steady_clock::now () + 1min)
        .result ();
    ASSERT_TRUE (result);
    ASSERT_EQ (provider.put_references.size (), 3u);
    EXPECT_EQ (provider.put_references[0], provider.put_references[1]);
    EXPECT_EQ (provider.put_references[0], provider.put_references[2]);
}

TEST (CppFrameworkOpaqueRelocationStore, ReconciliationDoesNotStopAtFixedAttemptCount)
{
    uncertain_put_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto result =
      repository
        .put_relocation (bytes ("eventual-commit"), 1h, std::chrono::steady_clock::now () + 1min)
        .result ();
    ASSERT_TRUE (result);
    ASSERT_EQ (provider.put_references.size (), 3u);
    EXPECT_EQ (provider.put_references[0], provider.put_references[1]);
    EXPECT_EQ (provider.put_references[0], provider.put_references[2]);
    ASSERT_EQ (provider.read_references.size (), 2u);
    EXPECT_EQ (provider.read_references[0], provider.put_references[0]);
    EXPECT_EQ (provider.read_references[1], provider.put_references[0]);
}

TEST (CppFrameworkOpaqueRelocationStore, ExpiredOperationDoesNotStartProviderIo)
{
    uncertain_put_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto result =
      repository.put_relocation (bytes ("expired"), 1h, std::chrono::steady_clock::now ())
        .result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::deadline_exceeded);
    EXPECT_TRUE (provider.put_references.empty ());
    EXPECT_TRUE (provider.read_references.empty ());
    try {
        (void) result.value ();
        FAIL () << "expired operation did not throw its typed failure";
    }
    catch (...) {
        const auto failure = detail::current_exception_result<void> ();
        EXPECT_EQ (failure.error_kind (), framework_error_kind_t::deadline_exceeded);
    }
}

class pending_relocation_store_t final : public relocation_store_t
{
  public:
    enum class stage_t
    {
        put,
        read
    };
    explicit pending_relocation_store_t (stage_t stage) : _stage (stage) {}
    std::future<void> started () { return _started.get_future (); }

    task_t<blob_put_result_t>
    put (blob_reference_t, std::span<const std::byte> payload, std::chrono::milliseconds) override
    {
        _payload.assign (payload.begin (), payload.end ());
        if (_stage == stage_t::read)
            return task_t<blob_put_result_t> (result_t<blob_put_result_t>::failure (
              framework_error_kind_t::unavailable, "put response uncertain"));
        _started.set_value ();
        return _put.task ();
    }
    task_t<blob_read_result_t> read (blob_reference_t) override
    {
        _started.set_value ();
        return _read.task ();
    }
    task_t<blob_renew_result_t> renew (blob_reference_t, std::chrono::milliseconds) override
    {
        return task_t<blob_renew_result_t> (
          result_t<blob_renew_result_t>::success (blob_missing_t{}));
    }
    task_t<void> erase (blob_reference_t) override
    {
        return task_t<void> (result_t<void>::success ());
    }
    void complete ()
    {
        const auto now = std::chrono::system_clock::now ();
        if (_stage == stage_t::put)
            _put.complete (result_t<blob_put_result_t>::success (blob_stored_t{now + 1h, now}));
        else
            _read.complete (
              result_t<blob_read_result_t>::success (blob_found_t{_payload, now + 1h, now}));
    }

  private:
    stage_t _stage;
    std::vector<std::byte> _payload;
    std::promise<void> _started;
    task_completion_source_t<blob_put_result_t> _put;
    task_completion_source_t<blob_read_result_t> _read;
};

void expect_pending_relocation_operation_deadline (pending_relocation_store_t::stage_t stage)
{
    pending_relocation_store_t provider (stage);
    provider_relocation_repository_t repository (provider);
    auto started = provider.started ();
    const auto timeout = 200ms;
    auto operation = std::async (std::launch::async, [&] {
        return repository
          .put_relocation (bytes ("pending"), 1h, std::chrono::steady_clock::now () + timeout)
          .result ();
    });
    const auto started_status = started.wait_for (5s);
    EXPECT_EQ (started_status, std::future_status::ready);
    EXPECT_EQ (operation.wait_for (timeout * 2), std::future_status::ready);
    provider.complete ();
    const auto result = operation.get ();
    EXPECT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::deadline_exceeded);
}

TEST (CppFrameworkOpaqueRelocationStore, PendingPutEndsAtOperationDeadline)
{
    expect_pending_relocation_operation_deadline (pending_relocation_store_t::stage_t::put);
}

TEST (CppFrameworkOpaqueRelocationStore, PendingReadBackEndsAtOperationDeadline)
{
    expect_pending_relocation_operation_deadline (pending_relocation_store_t::stage_t::read);
}

TEST (CppFrameworkOpaqueRelocationStore, CallerIssuedReferenceSupportsExactReconcile)
{
    in_memory_relocation_store_t store;
    const blob_reference_t reference{"relocation:operation-0001:chunk-0000"};
    const auto payload = bytes ("immutable");

    const auto stored = store.put (reference, payload, 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_stored_t> (stored));

    const auto replay = store.put (reference, payload, 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_already_stored_t> (replay));

    const auto conflict = store.put (reference, bytes ("changed"), 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_conflict_t> (conflict));

    const auto exact = store.read (reference).result ().value ();
    const auto *found = std::get_if<blob_found_t> (&exact);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);

    store.erase (reference).result ().value ();
    EXPECT_TRUE (
      std::holds_alternative<blob_missing_t> (store.read (reference).result ().value ()));
}

TEST (CppFrameworkOpaqueRelocationStore, PrivateRepositoryUsesTheRegisteredOpaqueProvider)
{
    in_memory_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto payload = bytes ("repository-payload");

    const auto stored =
      repository.put_relocation (payload, 1h, std::chrono::steady_clock::now () + 1min)
        .result ()
        .value ();
    EXPECT_FALSE (stored.reference.empty ());
    EXPECT_GT (stored.expires_at, stored.store_now);

    const auto provider_read =
      provider.read (blob_reference_t{stored.reference}).result ().value ();
    const auto *provider_found = std::get_if<blob_found_t> (&provider_read);
    ASSERT_NE (provider_found, nullptr);
    EXPECT_EQ (provider_found->bytes, payload);

    const auto repository_read = repository.get_relocation (stored.reference).result ().value ();
    const auto *repository_found = std::get_if<relocation_found_t> (&repository_read);
    ASSERT_NE (repository_found, nullptr);
    EXPECT_EQ (repository_found->payload, payload);

    const auto renewed = repository.renew_relocation (stored.reference, 2h).result ().value ();
    EXPECT_TRUE (std::holds_alternative<relocation_renewed_t> (renewed));

    EXPECT_EQ (repository.delete_relocation (stored.reference).result ().value (),
               relocation_delete_result_t::deleted);
    EXPECT_TRUE (std::holds_alternative<relocation_missing_t> (
      repository.get_relocation (stored.reference).result ().value ()));
}

TEST (CppFrameworkOpaqueRelocationStore, RepositoryReconcilesLostCommitReply)
{
    post_commit_failure_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto payload = bytes ("immutable-after-timeout");

    const auto stored =
      repository.put_relocation (payload, 1h, std::chrono::steady_clock::now () + 1min)
        .result ()
        .value ();
    const auto read = provider.inner.read (blob_reference_t{stored.reference}).result ().value ();
    const auto *found = std::get_if<blob_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);
}
}
