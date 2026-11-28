/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/relocation_completion_store.hpp"

#include "runtime/execution/task_result.hpp"
#include "runtime/locations/authority_aggregate_relocation.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/base64.hpp"
#include "runtime/locations/location_store_write.hpp"
#include "runtime/locations/provider_location_repository.hpp"

namespace zlink::framework::runtime::relocation_completion
{
namespace
{
namespace authority = authority_aggregate_codec;
using json_t = authority::json_t;

constexpr std::chrono::hours active_retention{24};
constexpr std::chrono::minutes terminal_retention{5};
constexpr std::array<std::string_view, 5> states{"accepted", "pending", "terminalReceived",
                                                 "alreadyTerminal", "sourceLeaseExpired"};

[[noreturn]] void data_lost (const char *message)
{
    throw framework_exception_t (framework_error_kind_t::data_lost, message);
}

bool valid_owner (std::string_view owner)
{
    return !owner.empty () && detail::is_valid_utf8 (owner);
}

std::string encode (const row_t &row)
{
    const auto &source = row.source;
    if (!detail::is_lowercase_uuid_text (row.relocation_id)
        || !authority_key_codec_detail::authority_key_kind (row.object_key)
        || !detail::is_valid_utf8 (row.object_key) || !valid_owner (source.owner_id)
        || source.lease_generation == 0
        || source.lease_generation > static_cast<std::uint64_t> (location_counter_max)
        || source.node_rid.empty () || source.node_rid.size () > 510
        || source.node_rid.size () % 2 != 0 || !authority::lower_hex (source.node_rid)
        || source.node_generation == 0 || row.operation_id.size () != detail::uuid_hex_length
        || !authority::lower_hex (row.operation_id)
        || row.operation_id.find_first_not_of ('0') == std::string::npos || row.reply_route_id == 0
        || !valid_owner (row.writer.owner_id) || row.writer.lease_generation <= 0
        || (row.state == delivery_t::accepted) != row.reply.empty ())
        data_lost ("Invalid relocation reply row");

    json_t result{{"formatVersion", 1},
                  {"objectKey", row.object_key},
                  {"operationId", row.operation_id},
                  {"relocationId", row.relocation_id}};
    if (row.state != delivery_t::accepted)
        result["reply"] = base64_encode (row.reply);
    result["replyRouteId"] = std::to_string (row.reply_route_id);
    result["source"] = {{"leaseGeneration", std::to_string (source.lease_generation)},
                        {"nodeGeneration", std::to_string (source.node_generation)},
                        {"nodeRid", source.node_rid},
                        {"ownerId", source.owner_id}};
    result["state"] = states.at (static_cast<std::size_t> (row.state));
    result["writer"] = {{"leaseGeneration", std::to_string (row.writer.lease_generation)},
                        {"ownerId", row.writer.owner_id}};
    return result.dump ();
}

std::vector<std::byte> bytes (std::string_view text)
{
    const auto octets = std::as_bytes (std::span (text));
    return {octets.begin (), octets.end ()};
}

row_t decode (const store_key_t &stored_key, const store_value_t &value)
{
    try {
        const std::string_view text{reinterpret_cast<const char *> (value.bytes.data ()),
                                    value.bytes.size ()};
        const auto data = json_t::parse (text);
        const auto &source = data.at ("source");
        const auto &writer = data.at ("writer");
        const auto writer_generation = authority::number (writer, "leaseGeneration");
        const auto state = std::ranges::find (states, authority::text (data, "state"));
        if (data.at ("formatVersion") != 1 || state == states.end ()
            || writer_generation > static_cast<std::uint64_t> (location_counter_max)
            || !value.expires_at)
            data_lost ("Invalid relocation reply row metadata");
        row_t row{
          authority::text (data, "relocationId"),
          authority::text (data, "objectKey"),
          {authority::text (source, "ownerId"), authority::number (source, "leaseGeneration"),
           authority::text (source, "nodeRid"), authority::number (source, "nodeGeneration")},
          authority::text (data, "operationId"),
          authority::number (data, "replyRouteId"),
          {authority::text (writer, "ownerId"), static_cast<std::int64_t> (writer_generation)},
          static_cast<delivery_t> (state - states.begin ())};
        if (row.state != delivery_t::accepted)
            row.reply = base64_decode (authority::text (data, "reply"));
        if (key (row).value != stored_key.value || encode (row) != text)
            data_lost ("Relocation reply row key or encoding disagrees with value");
        return row;
    }
    catch (const nlohmann::json::exception &) {
        data_lost ("Malformed relocation reply row");
    }
    catch (const std::invalid_argument &) {
        data_lost ("Malformed relocation reply row fields");
    }
}
}

std::string prefix (std::string_view relocation_id)
{
    return "$zlink/relocation-reply/" + std::string (relocation_id) + "/";
}

store_key_t key (const row_t &row)
{
    const auto &source = row.source;
    return {prefix (row.relocation_id) + source.owner_id + "."
            + std::to_string (source.lease_generation) + "." + source.node_rid + "."
            + std::to_string (source.node_generation) + "/" + row.operation_id};
}

store_put_t accepted (const row_t &row)
{
    return {key (row), bytes (encode (row)), active_retention};
}

bool append_accepted (store_write_request_t &batch, std::span<const store_put_t> rows)
{
    if (rows.empty ())
        return true;
    for (const auto &row : rows) {
        batch.conditions.emplace_back (store_missing_condition_t{row.key});
        batch.mutations.emplace_back (row);
    }
    try {
        validate_location_write (batch, [] (const auto &key) {
            if (key.empty () || key.size () > location_store_limits::key_bytes)
                throw std::invalid_argument ("Invalid Location Store key");
        });
        return true;
    }
    catch (const std::invalid_argument &) {
        return false;
    }
}

task_t<std::optional<row_t>> repository_t::record_result (row_t identity,
                                                          std::vector<std::byte> reply,
                                                          std::vector<std::byte> failure)
{
    return update (std::move (identity), delivery_t::pending, std::move (reply),
                   std::move (failure));
}

task_t<bool> repository_t::confirm (row_t identity, delivery_t state)
{
    if (state == delivery_t::source_lease_expired) {
        if (co_await source_lease_live (identity.source))
            co_return false;
    }
    const auto saved = co_await update (std::move (identity), state);
    co_return !saved || saved->state >= delivery_t::terminal_received;
}

task_t<bool> repository_t::source_lease_live (const source_t &source)
{
    return _locations.owner_is_live_async (
      {source.owner_id, static_cast<std::int64_t> (source.lease_generation)});
}

task_t<std::optional<row_t>> repository_t::update (row_t identity,
                                                   delivery_t state,
                                                   std::vector<std::byte> reply,
                                                   std::vector<std::byte> failure)
{
    const auto row_key = key (identity);
    for (;;) {
        const auto read = co_await _locations._store.read (row_key);
        const auto *found = std::get_if<store_found_t> (&read);
        if (!found)
            co_return std::nullopt;
        auto row = decode (row_key, found->value);
        if (row.writer.owner_id != identity.writer.owner_id
            || row.writer.lease_generation != identity.writer.lease_generation
            || row.reply_route_id != identity.reply_route_id
            || row.object_key != identity.object_key)
            data_lost ("Relocation reply row fence disagrees with request");
        if (row.state >= delivery_t::terminal_received
            || (row.state == delivery_t::accepted && state != delivery_t::pending))
            co_return std::move (row);
        if (!co_await _locations.owner_is_live_async (row.writer))
            co_return std::nullopt;

        const bool renew = row.state == state;
        if (renew && *found->value.expires_at - found->value.store_now > active_retention / 2)
            co_return std::move (row);
        if (row.state == delivery_t::accepted)
            row.reply = reply;
        row.state = state;
        auto encoded = encode (row);
        // Every terminal state retains the same reply bytes. Reserve the
        // longest state's spelling before choosing the one immutable reply.
        constexpr auto terminal_state_growth = states.back ().size () - states[1].size ();
        if (!renew && state == delivery_t::pending
            && encoded.size () > location_store_limits::value_bytes - terminal_state_growth) {
            row.reply = failure;
            encoded = encode (row);
        }
        if (encoded.size () > location_store_limits::value_bytes)
            data_lost ("Relocation reply row exceeds the Location Store value limit");
        const std::chrono::milliseconds retention =
          state == delivery_t::pending ? std::chrono::milliseconds (active_retention)
                                       : std::chrono::milliseconds (terminal_retention);
        store_write_request_t write{{store_version_condition_t{row_key, found->value.version},
                                     _locations.owner_condition (row.writer)},
                                    {store_put_t{row_key, bytes (encoded), retention}}};
        auto result = co_await await_result (_locations._store.write (std::move (write)));
        if (result) {
            if (std::holds_alternative<store_write_applied_t> (result.value ()))
                co_return std::move (row);
            if (std::holds_alternative<store_write_corrupt_t> (result.value ()))
                data_lost ("Location Store reported a corrupt relocation reply write");
            continue;
        }
        if (!detail::is_transient_error (result.error_kind ()))
            (void) result.value ();
        // Resolve the unknown outcome before the existing relay pump retries.
        const auto observed = co_await _locations._store.read (row_key);
        if (const auto *current = std::get_if<store_found_t> (&observed);
            current && current->value.version.value != found->value.version.value)
            continue;
        (void) result.value ();
    }
}

task_t<std::vector<row_t>> repository_t::pending (std::string relocation_id)
{
    store_scan_request_t request{prefix (relocation_id), {}, 1000};
    std::vector<row_t> output;
    do {
        const auto result = co_await _locations._store.scan (request);
        const auto *page = std::get_if<store_scan_page_t> (&result);
        if (!page)
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Relocation reply scan expired");
        for (const auto &item : page->items) {
            auto row = decode (item.key, item.value);
            if (row.state == delivery_t::pending)
                output.push_back (std::move (row));
        }
        request.cursor = page->next_cursor;
    } while (request.cursor);
    co_return output;
}

}
