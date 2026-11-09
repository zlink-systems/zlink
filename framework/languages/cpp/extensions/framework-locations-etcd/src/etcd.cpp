/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/locations/etcd.hpp>
#include "client.hpp"
#include "cursor.hpp"
#include <algorithm>

namespace zlink::framework::etcd
{
namespace
{
namespace wire = etcdserverpb;
using kv_t = mvccpb::KeyValue;
using namespace detail;

wire::RequestOp read_key (const std::string &key, std::int64_t revision = 0)
{
    wire::RequestOp request;
    request.mutable_request_range ()->set_key (key);
    request.mutable_request_range ()->set_revision (revision);
    return request;
}
const kv_t *one (const wire::RangeResponse &response)
{
    if (response.kvs_size () > 1)
        store_failure ();
    return response.kvs_size () == 0 ? nullptr : &response.kvs (0);
}
void check_response (const wire::TxnResponse &response, std::size_t count)
{
    if (!response.has_header () || response.header ().revision () <= 0
        || static_cast<std::size_t> (response.responses_size ()) != count)
        store_failure ();
    for (const auto &item : response.responses ())
        if (!item.has_response_range ())
            store_failure ();
}
std::uint64_t store_now (const wire::TxnResponse &response)
{
    if (response.responses_size () < 1 || !response.responses (0).has_response_range ())
        store_failure ();
    const auto *clock = one (response.responses (0).response_range ());
    if (!clock)
        store_failure ();
    return decode_clock (clock->value ());
}
std::optional<metadata_t> record (const kv_t *metadata, const kv_t *payload)
{
    if (!metadata && !payload)
        return std::nullopt;
    if (!metadata || !payload || metadata->mod_revision () <= 0
        || metadata->mod_revision () != payload->mod_revision ())
        store_failure ();
    const auto info = decode_metadata (metadata->value ());
    if (info.payload_length != payload->value ().size ())
        store_failure ();
    return info;
}
store_value_t store_value (const kv_t &metadata,
                           const kv_t &payload,
                           metadata_t info,
                           std::uint64_t cluster,
                           std::uint64_t now)
{
    return {to_bytes (payload.value ()),
            {encode_version (cluster, metadata.mod_revision ())},
            info.has_expiry ? std::optional{from_ms (info.expires_at)} : std::nullopt,
            from_ms (now)};
}
template <typename Variant> const store_key_t &key_of (const Variant &value)
{
    return std::visit ([] (const auto &item) -> const store_key_t & { return item.key; }, value);
}
wire::Compare exists (const std::string &key, bool present)
{
    wire::Compare compare;
    compare.set_key (key);
    compare.set_target (wire::Compare::CREATE);
    compare.set_result (present ? wire::Compare::GREATER : wire::Compare::EQUAL);
    compare.set_create_revision (0);
    return compare;
}
}

struct etcd_location_store_t::impl_t
{
    explicit impl_t (etcd_location_options_t options) :
        client (options),
        ns (std::move (options.key_namespace)),
        timeout (options.operation_timeout),
        clock (physical_key (ns, 'c'))
    {
    }

    struct operation_owner_t
    {
        explicit operation_owner_t (std::shared_ptr<impl_t> value) : self (std::move (value))
        {
            self->client.start ();
        }
        ~operation_owner_t () { self->client.finish (); }
        std::shared_ptr<impl_t> self;
    };

    template <typename T, typename Work>
    static task_t<T> run (std::shared_ptr<impl_t> self, Work work)
    {
        const auto lifetime = std::make_shared<operation_owner_t> (self);
        auto source = std::make_shared<task_completion_source_t<T>> ();
        auto task = source->task ();
        const operation_t operation (self->timeout, lifetime);
        auto pending = [&] {
            namespace execution = zlink::framework::detail;
            // The SPI has no cancellation token. Internal RPC waits do not
            // inherit a consumer's turn or wait cancellation.
            execution::serial_turn_scope_t turn (std::shared_ptr<execution::serial_turn_t>{});
            execution::ambient_context_scope_t context ({}, nullptr, {}, false, true);
            return work (operation);
        }();
        zlink::framework::detail::observe_task_terminal (
          pending, [source, operation] (const result_t<T> &result) {
              if (result && operation.expired ())
                  source->complete (result_t<T>::failure (framework_error_kind_t::internal_failure,
                                                          store_failure_message));
              else
                  source->complete (result);
          });
        return task;
    }

    std::string metadata_key (std::string_view key) const { return physical_key (ns, 'm', key); }
    std::string payload_key (std::string_view key) const { return physical_key (ns, 'p', key); }

    task_t<store_read_result_t> read (store_key_t key, operation_t operation)
    {
        wire::TxnRequest request;
        *request.add_success () = read_key (clock);
        *request.add_success () = read_key (metadata_key (key.value));
        *request.add_success () = read_key (payload_key (key.value));
        const auto response = co_await client.txn (std::move (request), operation);
        check_response (response, 3);
        const auto now = store_now (response);
        const auto *metadata = one (response.responses (1).response_range ());
        const auto *payload = one (response.responses (2).response_range ());
        const auto info = record (metadata, payload);
        if (!info || info->expired (now))
            co_return store_missing_t{from_ms (now)};
        co_return store_found_t{
          store_value (*metadata, *payload, *info, response.header ().cluster_id (), now)};
    }

    struct decision_t
    {
        std::uint64_t now;
        std::uint64_t cluster;
        std::int64_t revision;
        std::vector<std::optional<kv_t>> metadata;
        std::vector<std::optional<metadata_t>> info;
    };
    struct evaluation_t
    {
        bool matches = true;
        std::optional<std::uint64_t> minimum_expiry;
    };

    using write_input_t = std::shared_ptr<const store_write_request_t>;
    using decision_ptr_t = std::shared_ptr<const decision_t>;

    task_t<decision_ptr_t> decide (write_input_t request, operation_t operation)
    {
        wire::TxnRequest query;
        *query.add_success () = read_key (clock);
        for (const auto &condition : request->conditions)
            *query.add_success () = read_key (metadata_key (key_of (condition).value));
        const auto observed = co_await client.txn (std::move (query), operation);
        check_response (observed, request->conditions.size () + 1);
        auto result = std::make_shared<decision_t> (decision_t{store_now (observed),
                                                               observed.header ().cluster_id (),
                                                               observed.header ().revision (),
                                                               {},
                                                               {}});
        for (std::size_t i = 0; i < request->conditions.size (); ++i) {
            const auto *metadata =
              one (observed.responses (static_cast<int> (i + 1)).response_range ());
            result->metadata.push_back (metadata ? std::optional{*metadata} : std::nullopt);
            result->info.push_back (metadata ? std::optional{decode_metadata (metadata->value ())}
                                             : std::nullopt);
            if (metadata && metadata->mod_revision () <= 0)
                store_failure ();
        }
        co_return result;
    }

    task_t<std::vector<std::optional<kv_t>>>
    read_payloads (write_input_t request, decision_ptr_t decision, operation_t operation)
    {
        wire::TxnRequest query;
        std::vector<std::size_t> positions;
        for (std::size_t i = 0; i < request->conditions.size (); ++i) {
            const auto *condition = std::get_if<store_value_condition_t> (&request->conditions[i]);
            if (condition && decision->info[i]
                && decision->info[i]->payload_length == condition->expected.size ()) {
                positions.push_back (i);
                *query.add_success () =
                  read_key (payload_key (condition->key.value), decision->revision);
            }
        }
        std::vector<std::optional<kv_t>> values (request->conditions.size ());
        if (!positions.empty ()) {
            const auto response = co_await client.txn (std::move (query), operation);
            check_response (response, positions.size ());
            if (response.header ().cluster_id () != decision->cluster)
                store_failure ();
            for (std::size_t j = 0; j < positions.size (); ++j) {
                const auto i = positions[j];
                const auto *payload =
                  one (response.responses (static_cast<int> (j)).response_range ());
                record (&*decision->metadata[i], payload);
                values[i] = *payload;
            }
        }
        co_return values;
    }

    static evaluation_t evaluate (const store_write_request_t &request,
                                  const decision_t &decision,
                                  const std::vector<std::optional<kv_t>> &values)
    {
        evaluation_t result;
        for (std::size_t i = 0; i < request.conditions.size (); ++i) {
            const auto &info = decision.info[i];
            const bool live = info && !info->expired (decision.now);
            const bool matches = std::visit (
              [&] (const auto &condition) {
                  using T = std::decay_t<decltype (condition)>;
                  if constexpr (std::is_same_v<T, store_missing_condition_t>)
                      return !live;
                  else if constexpr (std::is_same_v<T, store_version_condition_t>) {
                      return live
                             && encode_version (decision.cluster,
                                                decision.metadata[i]->mod_revision ())
                                  == condition.expected.value;
                  } else
                      return live && values[i]
                             && values[i]->value () == to_string (condition.expected);
              },
              request.conditions[i]);
            if (!matches)
                return {false, {}};
            if (live && info->has_expiry
                && !std::holds_alternative<store_missing_condition_t> (request.conditions[i]))
                result.minimum_expiry = result.minimum_expiry
                                          ? std::min (*result.minimum_expiry, info->expires_at)
                                          : info->expires_at;
        }
        return result;
    }

    wire::TxnRequest apply_request (const store_write_request_t &request,
                                    const decision_t &decision,
                                    const evaluation_t &evaluation)
    {
        wire::TxnRequest apply;
        *apply.add_compare () = exists (clock, true);
        if (evaluation.minimum_expiry) {
            auto *compare = apply.add_compare ();
            compare->set_key (clock);
            compare->set_target (wire::Compare::VALUE);
            compare->set_result (wire::Compare::LESS);
            compare->set_value (encode_clock (*evaluation.minimum_expiry));
        }
        for (std::size_t i = 0; i < request.conditions.size (); ++i) {
            const auto key = metadata_key (key_of (request.conditions[i]).value);
            if (!decision.metadata[i])
                *apply.add_compare () = exists (key, false);
            else {
                auto *compare = apply.add_compare ();
                compare->set_key (key);
                compare->set_target (wire::Compare::MOD);
                compare->set_result (wire::Compare::EQUAL);
                compare->set_mod_revision (decision.metadata[i]->mod_revision ());
            }
        }
        for (const auto &mutation : request.mutations) {
            std::visit (
              [&] (const auto &item) {
                  if constexpr (std::is_same_v<std::decay_t<decltype (item)>, store_put_t>) {
                      const auto retention =
                        item.retention ? static_cast<std::uint64_t> (item.retention->count ()) : 0;
                      if (retention > max_time - decision.now)
                          throw std::invalid_argument ("Store expiry exceeds its bound.");
                      auto *metadata = apply.add_success ()->mutable_request_put ();
                      metadata->set_key (metadata_key (item.key.value));
                      metadata->set_value (encode_metadata (
                        {item.retention.has_value (), item.retention ? decision.now + retention : 0,
                         static_cast<std::uint32_t> (item.bytes.size ())}));
                      auto *payload = apply.add_success ()->mutable_request_put ();
                      payload->set_key (payload_key (item.key.value));
                      payload->set_value (to_string (item.bytes));
                  } else {
                      apply.add_success ()->mutable_request_delete_range ()->set_key (
                        metadata_key (item.key.value));
                      apply.add_success ()->mutable_request_delete_range ()->set_key (
                        payload_key (item.key.value));
                  }
              },
              mutation);
        }
        return apply;
    }

    static store_write_applied_t applied (const store_write_request_t &request,
                                          std::uint64_t now,
                                          const wire::TxnResponse &response)
    {
        store_write_applied_t result{{}, from_ms (now)};
        for (const auto &mutation : request.mutations)
            if (const auto *put = std::get_if<store_put_t> (&mutation))
                result.put_versions.push_back ({put->key,
                                                {encode_version (response.header ().cluster_id (),
                                                                 response.header ().revision ())}});
        return result;
    }

    task_t<store_write_result_t> write (store_write_request_t request, operation_t operation)
    {
        const auto input = std::make_shared<const store_write_request_t> (std::move (request));
        for (;;) {
            operation.check ();
            const auto decision = co_await decide (input, operation);
            std::vector<std::optional<kv_t>> payloads;
            bool compacted = false;
            try {
                payloads = co_await read_payloads (input, decision, operation);
            }
            catch (const call_failure_t &error) {
                if (error.kind != error_kind_t::compaction)
                    throw;
                compacted = true;
            }
            if (compacted)
                continue;
            const auto evaluation = evaluate (*input, *decision, payloads);
            if (!evaluation.matches)
                co_return store_write_conflict_t{from_ms (decision->now)};
            auto response =
              co_await client.txn (apply_request (*input, *decision, evaluation), operation);
            if (!response.has_header () || response.header ().cluster_id () != decision->cluster)
                store_failure ();
            if (response.succeeded ())
                co_return applied (*input, decision->now, response);
        }
    }

    struct selected_t
    {
        std::string key;
        kv_t metadata;
        metadata_t info;
    };

    task_t<store_scan_result_t>
    scan_snapshot (store_scan_request_t request, snapshot_t snapshot, operation_t operation)
    {
        std::vector<selected_t> selected;
        std::size_t size = 0;
        bool more = false;
        const auto physical_prefix = metadata_key (request.prefix);
        for (;;) {
            if (snapshot.expired ())
                co_return store_scan_expired_t{};
            wire::RangeRequest query;
            const auto start = snapshot.last.empty ()
                                 ? physical_prefix
                                 : metadata_key (snapshot.last) + std::string (1, '\0');
            query.set_key (start);
            query.set_range_end (prefix_end (physical_prefix));
            query.set_revision (snapshot.revision);
            query.set_limit (1000);
            query.set_sort_order (wire::RangeRequest::ASCEND);
            query.set_sort_target (wire::RangeRequest::KEY);
            const auto range = co_await client.range (std::move (query), operation);
            if (!range.has_header ())
                store_failure ();
            if (range.header ().cluster_id () != snapshot.cluster)
                co_return store_scan_expired_t{};
            std::string previous;
            for (const auto &metadata : range.kvs ()) {
                if (!metadata.key ().starts_with (physical_prefix) || metadata.key () < start
                    || (!previous.empty () && metadata.key () <= previous))
                    store_failure ();
                previous = metadata.key ();
                const auto key = metadata.key ().substr (metadata_key ({}).size ());
                if (key.empty () || key.size () > 1024 || !is_valid_utf8 (key))
                    store_failure ();
                const auto info = decode_metadata (metadata.value ());
                if (info.expired (snapshot.now)) {
                    snapshot.last = key;
                    continue;
                }
                const auto item_size =
                  key.size () + info.payload_length
                  + encode_version (snapshot.cluster, metadata.mod_revision ()).size () + 128;
                if (selected.size () == request.limit || item_size > max_page_bytes - size) {
                    more = true;
                    break;
                }
                selected.push_back ({key, metadata, info});
                size += item_size;
                snapshot.last = key;
            }
            if (more || !range.more ())
                break;
            if (range.kvs_size () == 0)
                store_failure ();
        }
        wire::TxnRequest query;
        for (const auto &item : selected)
            *query.add_success () = read_key (payload_key (item.key), snapshot.revision);
        store_scan_page_t page{{}, {}, from_ms (snapshot.now)};
        if (!selected.empty ()) {
            const auto response = co_await client.txn (std::move (query), operation);
            check_response (response, selected.size ());
            if (response.header ().cluster_id () != snapshot.cluster)
                co_return store_scan_expired_t{};
            for (std::size_t i = 0; i < selected.size (); ++i) {
                const auto &item = selected[i];
                const auto *payload =
                  one (response.responses (static_cast<int> (i)).response_range ());
                record (&item.metadata, payload);
                page.items.push_back ({{item.key},
                                       store_value (item.metadata, *payload, item.info,
                                                    snapshot.cluster, snapshot.now)});
            }
        }
        if (snapshot.expired ())
            co_return store_scan_expired_t{};
        if (more)
            page.next_cursor = store_scan_cursor_t{cursors.encode (snapshot, ns, request.prefix)};
        co_return page;
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request, operation_t operation)
    {
        std::optional<snapshot_t> previous;
        if (request.cursor) {
            previous = cursors.decode (request.cursor->value, ns, request.prefix);
            if (!previous)
                co_return store_scan_expired_t{};
        }
        const auto started = monotonic_ms ();
        wire::TxnRequest query;
        *query.add_success () = read_key (clock);
        const auto observed = co_await client.txn (std::move (query), operation);
        check_response (observed, 1);
        const auto now = store_now (observed);
        if (previous && previous->cluster != observed.header ().cluster_id ())
            co_return store_scan_expired_t{};
        const auto snapshot = previous.value_or (snapshot_t{
          observed.header ().cluster_id (), observed.header ().revision (), now, started, {}});
        try {
            co_return co_await scan_snapshot (std::move (request), snapshot, operation);
        }
        catch (const call_failure_t &error) {
            if (error.kind != error_kind_t::compaction)
                throw;
        }
        co_return store_scan_expired_t{};
    }

    client_t client;
    std::string ns;
    std::chrono::milliseconds timeout;
    std::string clock;
    cursor_t cursors;
};

etcd_location_store_t::etcd_location_store_t (etcd_location_options_t options) :
    _impl (std::make_shared<impl_t> (std::move (options)))
{
}
etcd_location_store_t::~etcd_location_store_t ()
{
    _impl->client.close ();
}

task_t<store_read_result_t> etcd_location_store_t::read (store_key_t key)
{
    detail::validate_key (key.value);
    return impl_t::run<store_read_result_t> (_impl, [&] (detail::operation_t operation) {
        return _impl->read (std::move (key), operation);
    });
}
task_t<store_write_result_t> etcd_location_store_t::write (store_write_request_t request)
{
    detail::validate_write (request);
    return impl_t::run<store_write_result_t> (_impl, [&] (detail::operation_t operation) {
        return _impl->write (std::move (request), operation);
    });
}
task_t<store_scan_result_t> etcd_location_store_t::scan (store_scan_request_t request)
{
    detail::validate_key (request.prefix, true);
    if (request.limit == 0 || request.limit > 1000)
        throw std::invalid_argument ("Store scan limit must be 1..1000.");
    if (request.cursor
        && (request.cursor->value.empty () || request.cursor->value.size () > 4096
            || !detail::is_valid_utf8 (request.cursor->value)))
        throw std::invalid_argument ("Invalid Store cursor length.");
    return impl_t::run<store_scan_result_t> (_impl, [&] (detail::operation_t operation) {
        return _impl->scan (std::move (request), operation);
    });
}
} // namespace zlink::framework::etcd
