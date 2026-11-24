/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/locations/etcd.hpp>
#include "client.hpp"
#include "cursor.hpp"
#include "runtime/locations/location_store_write.hpp"
#include <algorithm>
#include <map>

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

    struct observed_record_t
    {
        bool counter = false;
        const store_value_condition_t *value_condition = nullptr;
        std::optional<kv_t> metadata;
        std::optional<metadata_t> info;
        std::optional<kv_t> payload;

        bool live (std::uint64_t now) const { return info && !info->expired (now); }

        bool needs_payload (std::uint64_t now) const
        {
            return info
                   && ((counter && live (now)
                        && info->payload_length <= runtime::location_counter_digits)
                       || (value_condition
                           && info->payload_length == value_condition->expected.size ()));
        }
    };

    struct decision_t
    {
        std::uint64_t now = 0;
        std::uint64_t cluster = 0;
        std::int64_t revision = 0;
        std::map<std::string_view, observed_record_t> records;

        std::optional<std::int64_t> counter (const store_key_t &key) const
        {
            const auto &record = records.at (key.value);
            if (!record.live (now))
                return 0;
            // A live counter larger than 19 bytes cannot hold a valid int64.
            if (!record.payload)
                return std::nullopt;
            return runtime::parse_location_counter (record.payload->value ());
        }
    };

    using write_input_t = std::shared_ptr<const store_write_request_t>;
    using decision_ptr_t = std::shared_ptr<decision_t>;

    task_t<decision_ptr_t> decide (write_input_t request, operation_t operation)
    {
        auto result = std::make_shared<decision_t> ();
        for (const auto &condition : request->conditions) {
            const bool counter = std::holds_alternative<store_sum_at_most_condition_t> (condition);
            const auto *value = std::get_if<store_value_condition_t> (&condition);
            runtime::for_each_condition_key (condition, [&] (const store_key_t &key) {
                auto &record = result->records[key.value];
                record.counter = counter;
                record.value_condition = value;
            });
        }
        for (const auto &mutation : request->mutations)
            if (const auto *add = std::get_if<store_add_t> (&mutation))
                result->records[add->key.value].counter = true;

        wire::TxnRequest query;
        *query.add_success () = read_key (clock);
        for (const auto &[key, record] : result->records)
            *query.add_success () = read_key (metadata_key (key));
        const auto observed = co_await client.txn (std::move (query), operation);
        check_response (observed, result->records.size () + 1);
        result->now = store_now (observed);
        result->cluster = observed.header ().cluster_id ();
        result->revision = observed.header ().revision ();
        int position = 1;
        for (auto &[key, entry] : result->records) {
            const auto *metadata = one (observed.responses (position++).response_range ());
            if (metadata) {
                if (metadata->mod_revision () <= 0)
                    store_failure ();
                entry.metadata = *metadata;
                entry.info = decode_metadata (metadata->value ());
            }
        }
        co_return result;
    }

    task_t<decision_ptr_t> read_payloads (decision_ptr_t decision, operation_t operation)
    {
        wire::TxnRequest query;
        std::vector<observed_record_t *> entries;
        for (auto &[key, entry] : decision->records) {
            if (entry.needs_payload (decision->now)) {
                entries.push_back (&entry);
                *query.add_success () = read_key (payload_key (key), decision->revision);
            }
        }
        if (!entries.empty ()) {
            const auto response = co_await client.txn (std::move (query), operation);
            check_response (response, entries.size ());
            if (response.header ().cluster_id () != decision->cluster)
                store_failure ();
            for (std::size_t i = 0; i < entries.size (); ++i) {
                auto &entry = *entries[i];
                const auto *payload =
                  one (response.responses (static_cast<int> (i)).response_range ());
                record (&*entry.metadata, payload);
                entry.payload = *payload;
            }
        }
        co_return decision;
    }

    static runtime::location_sum_result_t evaluate (const store_write_request_t &request,
                                                    const decision_t &decision)
    {
        using result_t = runtime::location_sum_result_t;
        for (const auto &condition : request.conditions) {
            const auto result = std::visit (
              [&] (const auto &item) {
                  using T = std::decay_t<decltype (item)>;
                  if constexpr (std::is_same_v<T, store_sum_at_most_condition_t>) {
                      return runtime::evaluate_location_sum (
                        item, [&] (const store_key_t &key) { return decision.counter (key); });
                  } else {
                      const auto &record = decision.records.at (item.key.value);
                      const bool live = record.live (decision.now);
                      bool matches;
                      if constexpr (std::is_same_v<T, store_missing_condition_t>)
                          matches = !live;
                      else if constexpr (std::is_same_v<T, store_version_condition_t>)
                          matches =
                            live
                            && encode_version (decision.cluster, record.metadata->mod_revision ())
                                 == item.expected.value;
                      else
                          matches = live && record.payload
                                    && record.payload->value () == to_string (item.expected);
                      return matches ? result_t::matches : result_t::exceeds;
                  }
              },
              condition);
            if (result != result_t::matches)
                return result;
        }
        return result_t::matches;
    }

    std::optional<wire::TxnRequest> apply_request (const store_write_request_t &request,
                                                   const decision_t &decision)
    {
        wire::TxnRequest apply;
        *apply.add_compare () = exists (clock, true);
        std::optional<std::uint64_t> minimum_expiry;
        for (const auto &[key, record] : decision.records) {
            const auto physical = metadata_key (key);
            if (!record.metadata)
                *apply.add_compare () = exists (physical, false);
            else {
                auto *compare = apply.add_compare ();
                compare->set_key (physical);
                compare->set_target (wire::Compare::MOD);
                compare->set_result (wire::Compare::EQUAL);
                compare->set_mod_revision (record.metadata->mod_revision ());
            }
            if (record.live (decision.now) && record.info->has_expiry
                && (!minimum_expiry || record.info->expires_at < *minimum_expiry))
                minimum_expiry = record.info->expires_at;
        }
        if (minimum_expiry) {
            auto *compare = apply.add_compare ();
            compare->set_key (clock);
            compare->set_target (wire::Compare::VALUE);
            compare->set_result (wire::Compare::LESS);
            compare->set_value (encode_clock (*minimum_expiry));
        }

        const auto append_put = [&] (const store_put_t &put) {
            const auto retention =
              put.retention ? static_cast<std::uint64_t> (put.retention->count ()) : 0;
            if (retention > max_time - decision.now)
                throw std::invalid_argument ("Store expiry exceeds its bound.");
            auto *metadata = apply.add_success ()->mutable_request_put ();
            metadata->set_key (metadata_key (put.key.value));
            metadata->set_value (encode_metadata (
              {put.retention.has_value (), put.retention ? decision.now + retention : 0,
               static_cast<std::uint32_t> (put.bytes.size ())}));
            auto *payload = apply.add_success ()->mutable_request_put ();
            payload->set_key (payload_key (put.key.value));
            payload->set_value (to_string (put.bytes));
        };
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<store_put_t> (&mutation))
                append_put (*put);
            else if (const auto *add = std::get_if<store_add_t> (&mutation)) {
                const auto put = runtime::prepare_location_add (*add, decision.counter (add->key));
                if (!put)
                    return std::nullopt;
                append_put (*put);
            } else {
                const auto &key = std::get<store_delete_t> (mutation).key.value;
                apply.add_success ()->mutable_request_delete_range ()->set_key (metadata_key (key));
                apply.add_success ()->mutable_request_delete_range ()->set_key (payload_key (key));
            }
        }
        return apply;
    }

    static store_write_applied_t applied (const store_write_request_t &request,
                                          std::uint64_t now,
                                          const wire::TxnResponse &response)
    {
        store_write_applied_t result{{}, from_ms (now)};
        for (const auto &mutation : request.mutations)
            std::visit (
              [&] (const auto &item) {
                  if constexpr (!std::is_same_v<std::decay_t<decltype (item)>, store_delete_t>)
                      result.put_versions.push_back (
                        {item.key,
                         {encode_version (response.header ().cluster_id (),
                                          response.header ().revision ())}});
              },
              mutation);
        return result;
    }

    task_t<store_write_result_t> write (store_write_request_t request, operation_t operation)
    {
        const auto input = std::make_shared<const store_write_request_t> (std::move (request));
        for (;;) {
            operation.check ();
            auto decision = co_await decide (input, operation);
            bool compacted = false;
            try {
                decision = co_await read_payloads (decision, operation);
            }
            catch (const call_failure_t &error) {
                if (error.kind != error_kind_t::compaction)
                    throw;
                compacted = true;
            }
            if (compacted)
                continue;
            const auto now = from_ms (decision->now);
            const auto evaluation = evaluate (*input, *decision);
            if (evaluation == runtime::location_sum_result_t::exceeds)
                co_return store_write_conflict_t{now};
            if (evaluation == runtime::location_sum_result_t::corrupt)
                co_return store_write_corrupt_t{now};
            auto apply = apply_request (*input, *decision);
            if (!apply)
                co_return store_write_corrupt_t{now};
            auto response = co_await client.txn (std::move (*apply), operation);
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
