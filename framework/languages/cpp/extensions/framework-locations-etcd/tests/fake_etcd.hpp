/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <etcd/api/etcdserverpb/rpc.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <zlink/locations/etcd.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace etcd_test
{
using namespace etcdserverpb;
using kv_t = mvccpb::KeyValue;

inline std::string clock_bytes (std::uint64_t value)
{
    std::string bytes (8, '\0');
    for (int i = 7; i >= 0; --i) {
        bytes[static_cast<std::size_t> (i)] = static_cast<char> (value & 255);
        value >>= 8;
    }
    return bytes;
}

inline std::string metadata (bool expiry, std::uint64_t time, std::uint32_t length)
{
    std::string bytes{"\x01\x00", 2};
    bytes[1] = expiry ? '\x01' : '\0';
    bytes += clock_bytes (time);
    for (int i = 3; i >= 0; --i)
        bytes += static_cast<char> ((length >> (i * 8)) & 255);
    return bytes;
}

inline std::string key (std::string_view ns, char kind, std::string_view suffix = {})
{
    std::string result (ns);
    result += '\0';
    result += kind;
    if (kind != 'c') {
        result += '\0';
        result += suffix;
    }
    return result;
}

inline bool applying (const TxnRequest &request)
{
    for (const auto &op : request.success ())
        if (op.has_request_put () || op.has_request_delete_range ())
            return true;
    return false;
}

inline bool historical (const TxnRequest &request)
{
    for (const auto &op : request.success ())
        if (op.has_request_range () && op.request_range ().revision () > 0)
            return true;
    return false;
}

// The server owns MVCC history; test faults run outside its state mutex.
class fake_etcd_t final : public KV::Service, public Auth::Service
{
  public:
    using txn_fault_t =
      std::function<grpc::Status (grpc::ServerContext *, const TxnRequest &, TxnResponse *)>;
    using range_fault_t =
      std::function<grpc::Status (grpc::ServerContext *, const RangeRequest &, RangeResponse *)>;
    using auth_fault_t =
      std::function<grpc::Status (grpc::ServerContext *, AuthenticateResponse *)>;

    explicit fake_etcd_t (
      std::shared_ptr<grpc::ServerCredentials> credentials = grpc::InsecureServerCredentials ())
    {
        grpc::ServerBuilder builder;
        // Distinguish the provider's send limit from server-side rejection.
        builder.SetMaxReceiveMessageSize (32 * 1024 * 1024);
        builder.AddListeningPort ("127.0.0.1:0", std::move (credentials), &_port);
        builder.RegisterService (static_cast<KV::Service *> (this));
        builder.RegisterService (static_cast<Auth::Service *> (this));
        _server = builder.BuildAndStart ();
        if (!_server || !_port)
            throw std::runtime_error ("fake etcd server failed to start");
        clock (1000);
    }

    ~fake_etcd_t () override
    {
        _server->Shutdown (std::chrono::system_clock::now () + std::chrono::seconds (2));
        _server->Wait ();
    }

    std::string endpoint (bool secure = false, bool localhost = true) const
    {
        return std::string (secure ? "https://" : "http://")
               + (localhost ? "localhost:" : "127.0.0.1:") + std::to_string (_port);
    }

    zlink::framework::etcd::etcd_location_options_t options () const
    {
        return {.endpoints = {endpoint ()}, .key_namespace = "test"};
    }

    grpc::Status
    Txn (grpc::ServerContext *context, const TxnRequest *request, TxnResponse *response) override
    {
        ++txn_calls;
        record_deadline (*context);
        return txn_fault ? txn_fault (context, *request, response) : transact (*request, response);
    }

    grpc::Status Range (grpc::ServerContext *context,
                        const RangeRequest *request,
                        RangeResponse *response) override
    {
        ++range_calls;
        record_deadline (*context);
        return range_fault ? range_fault (context, *request, response)
                           : respond_range (*request, response);
    }

    grpc::Status Authenticate (grpc::ServerContext *context,
                               const AuthenticateRequest *,
                               AuthenticateResponse *response) override
    {
        const auto number = ++auth_calls;
        record_deadline (*context);
        if (auth_fault) {
            const auto status = auth_fault (context, response);
            if (!status.ok ())
                return status;
        }
        response->set_token ("token-" + std::to_string (number));
        return grpc::Status::OK;
    }

    void clock (std::uint64_t time, std::string_view ns = "test")
    {
        put (key (ns, 'c'), clock_bytes (time));
    }

    void record (std::string_view suffix,
                 std::string payload,
                 std::optional<std::uint64_t> expiry = {},
                 std::string_view ns = "test")
    {
        std::lock_guard lock (_mutex);
        ++revision;
        write (key (ns, 'm', suffix),
               metadata (expiry.has_value (), expiry.value_or (0),
                         static_cast<std::uint32_t> (payload.size ())),
               revision);
        write (key (ns, 'p', suffix), std::move (payload), revision);
        save ();
    }

    void put (std::string name, std::string value)
    {
        std::lock_guard lock (_mutex);
        write (std::move (name), std::move (value), ++revision);
        save ();
    }

    void raw_record (std::optional<std::string> meta,
                     std::optional<std::string> payload,
                     std::int64_t meta_revision = 2,
                     std::int64_t payload_revision = 2)
    {
        std::lock_guard lock (_mutex);
        _records.erase (key ("test", 'm', "k"));
        _records.erase (key ("test", 'p', "k"));
        if (meta)
            write (key ("test", 'm', "k"), *meta, meta_revision);
        if (payload)
            write (key ("test", 'p', "k"), *payload, payload_revision);
        ++revision;
        save ();
    }

    void erase (const std::string &name)
    {
        std::lock_guard lock (_mutex);
        _records.erase (name);
        ++revision;
        save ();
    }

    std::optional<kv_t> get (const std::string &name)
    {
        std::lock_guard lock (_mutex);
        const auto found = _records.find (name);
        return found == _records.end () ? std::nullopt : std::optional<kv_t> (*found->second);
    }

    grpc::Status transact (const TxnRequest &request, TxnResponse *response)
    {
        std::lock_guard lock (_mutex);
        bool succeeded = true;
        for (const auto &comparison : request.compare ())
            succeeded = succeeded && compare (comparison);
        const auto &operations = succeeded ? request.success () : request.failure ();
        bool writing = false;
        for (const auto &op : operations)
            writing = writing || op.has_request_put () || op.has_request_delete_range ();
        if (writing)
            ++revision;
        response->set_succeeded (succeeded);
        for (const auto &op : operations) {
            auto *result = response->add_responses ();
            if (op.has_request_range ()) {
                const auto status = range (op.request_range (), result->mutable_response_range ());
                if (!status.ok ())
                    return status;
            } else if (op.has_request_put ()) {
                write (op.request_put ().key (), op.request_put ().value (), revision);
                result->mutable_response_put ();
            } else if (op.has_request_delete_range ()) {
                _records.erase (op.request_delete_range ().key ());
                result->mutable_response_delete_range ();
            } else {
                return {grpc::StatusCode::INVALID_ARGUMENT, "unsupported fake etcd transaction"};
            }
        }
        if (writing)
            save ();
        header (response->mutable_header ());
        return grpc::Status::OK;
    }

    grpc::Status respond_range (const RangeRequest &request, RangeResponse *response)
    {
        std::lock_guard lock (_mutex);
        return range (request, response);
    }

    std::vector<std::chrono::system_clock::time_point> deadlines ()
    {
        std::lock_guard lock (_deadlines_mutex);
        return _deadlines;
    }

    std::atomic<int> txn_calls{0}, range_calls{0}, auth_calls{0};
    std::atomic<std::int64_t> compacted{0};
    std::int64_t revision = 1;
    std::atomic<std::uint64_t> cluster{42};
    txn_fault_t txn_fault;
    range_fault_t range_fault;
    auth_fault_t auth_fault;

  private:
    using records_t = std::map<std::string, std::shared_ptr<const kv_t>, std::less<>>;

    void record_deadline (const grpc::ServerContext &context)
    {
        std::lock_guard lock (_deadlines_mutex);
        _deadlines.push_back (context.deadline ());
    }

    void header (ResponseHeader *target) const
    {
        target->set_cluster_id (cluster.load ());
        target->set_revision (revision);
    }

    void write (std::string name, std::string value, std::int64_t rev)
    {
        auto item = std::make_shared<kv_t> ();
        const auto old = _records.find (name);
        item->set_key (name);
        item->set_value (std::move (value));
        item->set_create_revision (old == _records.end () ? rev : old->second->create_revision ());
        item->set_mod_revision (rev);
        item->set_version (old == _records.end () ? 1 : old->second->version () + 1);
        _records[std::move (name)] = std::move (item);
    }

    void save () { _history[revision] = _records; }

    bool compare (const Compare &comparison) const
    {
        const auto position = _records.find (comparison.key ());
        const auto *current = position == _records.end () ? nullptr : position->second.get ();
        int order = 0;
        if (comparison.target () == Compare::VALUE) {
            order = (current ? current->value () : std::string{}).compare (comparison.value ());
        } else {
            std::int64_t actual = 0, expected = 0;
            switch (comparison.target ()) {
                case Compare::CREATE:
                    actual = current ? current->create_revision () : 0;
                    expected = comparison.create_revision ();
                    break;
                case Compare::MOD:
                    actual = current ? current->mod_revision () : 0;
                    expected = comparison.mod_revision ();
                    break;
                case Compare::VERSION:
                    actual = current ? current->version () : 0;
                    expected = comparison.version ();
                    break;
                default:
                    throw std::logic_error ("unsupported fake etcd comparison");
            }
            order = actual < expected ? -1 : actual > expected ? 1 : 0;
        }
        switch (comparison.result ()) {
            case Compare::EQUAL:
                return order == 0;
            case Compare::NOT_EQUAL:
                return order != 0;
            case Compare::LESS:
                return order < 0;
            case Compare::GREATER:
                return order > 0;
            default:
                return false;
        }
    }

    grpc::Status range (const RangeRequest &request, RangeResponse *response) const
    {
        if (request.revision () > 0 && request.revision () <= compacted.load ())
            return {grpc::StatusCode::OUT_OF_RANGE,
                    "etcdserver: mvcc: required revision has been compacted"};
        const records_t *source = &_records;
        if (request.revision () > 0) {
            auto position = _history.upper_bound (request.revision ());
            if (position == _history.begin ())
                return {grpc::StatusCode::OUT_OF_RANGE, "required revision is a future revision"};
            source = &std::prev (position)->second;
        }
        header (response->mutable_header ());
        if (request.range_end ().empty ()) {
            const auto position = source->find (request.key ());
            if (position != source->end ())
                *response->add_kvs () = *position->second;
        } else {
            const auto end = request.range_end () == std::string (1, '\0')
                               ? source->end ()
                               : source->lower_bound (request.range_end ());
            for (auto position = source->lower_bound (request.key ()); position != end;
                 ++position) {
                if (request.limit () > 0 && response->kvs_size () >= request.limit ()) {
                    response->set_more (true);
                    break;
                }
                *response->add_kvs () = *position->second;
            }
        }
        response->set_count (response->kvs_size ());
        return grpc::Status::OK;
    }

    int _port = 0;
    std::unique_ptr<grpc::Server> _server;
    std::mutex _mutex;
    records_t _records;
    std::map<std::int64_t, records_t> _history;
    std::mutex _deadlines_mutex;
    std::vector<std::chrono::system_clock::time_point> _deadlines;
};
} // namespace etcd_test
