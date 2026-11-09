/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "encoding.hpp"
#include <etcd/api/etcdserverpb/rpc.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <grpc/support/time.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace zlink::framework::etcd::detail
{
enum class error_kind_t
{
    compaction,
    no_space,
    auth,
    store_failure
};
error_kind_t classify (const grpc::Status &status);
struct call_failure_t : framework_exception_t
{
    explicit call_failure_t (error_kind_t value) :
        framework_exception_t (framework_error_kind_t::internal_failure, store_failure_message),
        kind (value)
    {
    }
    error_kind_t kind;
};

struct operation_t
{
    std::shared_ptr<void> lifetime;
    gpr_timespec deadline;
    operation_t (std::chrono::milliseconds timeout, std::shared_ptr<void> lifetime_owner);
    bool expired () const;
    void check () const;
};

class client_t
{
  public:
    explicit client_t (const etcd_location_options_t &options);
    ~client_t ();
    client_t (const client_t &) = delete;
    client_t &operator= (const client_t &) = delete;
    void start ();
    void finish ();
    void close ();
    task_t<etcdserverpb::TxnResponse> txn (etcdserverpb::TxnRequest request, operation_t operation);
    task_t<etcdserverpb::RangeResponse> range (etcdserverpb::RangeRequest request,
                                               operation_t operation);

  private:
    struct tag_t
    {
        virtual ~tag_t () = default;
        virtual void finish (bool ok) = 0;
    };
    struct queue_t
    {
        grpc::CompletionQueue queue;
        std::thread worker;
        std::mutex mutex;
        std::condition_variable idle;
        std::size_t active = 0;
        bool closing = false;
    };
    struct endpoint_t
    {
        std::unique_ptr<etcdserverpb::KV::Stub> kv;
        std::unique_ptr<etcdserverpb::Auth::Stub> auth;
    };
    template <typename R> struct response_t
    {
        R value;
        grpc::Status status;
    };
    template <typename R> struct call_t final : tag_t
    {
        // Destroyed after the RPC context and reader.
        std::shared_ptr<void> lifetime;
        grpc::ClientContext context;
        response_t<R> response;
        std::unique_ptr<grpc::ClientAsyncResponseReader<R>> reader;
        task_completion_source_t<response_t<R>> completion;
        client_t *owner;
        std::size_t endpoint;
        void finish (bool ok) override
        {
            std::unique_ptr<call_t> self (this);
            if (!ok && response.status.ok ())
                response.status = grpc::Status (grpc::StatusCode::UNKNOWN, "completion failed");
            if (response.status.error_code () == grpc::StatusCode::UNAVAILABLE
                || response.status.error_code () == grpc::StatusCode::DEADLINE_EXCEEDED) {
                auto expected = endpoint;
                owner->_endpoint.compare_exchange_strong (expected, (endpoint + 1)
                                                                      % owner->_endpoints.size ());
            }
            completion.complete (result_t<response_t<R>>::success (std::move (response)));
        }
    };

    template <typename R, typename Q, typename Start>
    task_t<response_t<R>>
    invoke (const Q &request, operation_t operation, bool authenticated, Start start)
    {
        operation.check ();
        auto call = std::make_unique<call_t<R>> ();
        call->lifetime = operation.lifetime;
        call->owner = this;
        call->endpoint = _endpoint.load ();
        call->context.set_deadline (operation.deadline);
        if (authenticated) {
            std::lock_guard lock (_token_mutex);
            if (!_token.empty ())
                call->context.AddMetadata ("token", _token);
        }
        auto result = call->completion.task ();
        call->reader = start (_endpoints[call->endpoint], &call->context, request, &_queue->queue);
        auto *tag = call.release ();
        tag->reader->Finish (&tag->response.value, &tag->response.status,
                             static_cast<tag_t *> (tag));
        return result;
    }

    template <typename R, typename Q, typename Start>
    task_t<R> call (Q request, operation_t operation, Start start)
    {
        bool needs_auth;
        {
            std::lock_guard lock (_token_mutex);
            needs_auth = _credentials && _token.empty ();
        }
        if (needs_auth)
            co_await authenticate (operation);
        auto response = co_await invoke<R> (request, operation, true, start);
        if (!response.status.ok () && classify (response.status) == error_kind_t::auth
            && _credentials) {
            co_await authenticate (operation);
            response = co_await invoke<R> (request, operation, true, start);
        }
        if (!response.status.ok ())
            throw call_failure_t (classify (response.status));
        co_return std::move (response.value);
    }
    task_t<void> authenticate (operation_t operation);
    std::shared_ptr<queue_t> _queue;
    std::vector<endpoint_t> _endpoints;
    std::atomic<std::size_t> _endpoint{0};
    std::optional<etcd_credentials_t> _credentials;
    std::mutex _token_mutex;
    std::string _token;
};
} // namespace zlink::framework::etcd::detail
