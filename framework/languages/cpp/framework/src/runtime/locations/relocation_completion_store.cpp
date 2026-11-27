/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/relocation_completion_store.hpp"

#include "runtime/locations/authority_aggregate_relocation.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/base64.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include <service_wire_codec.hpp>
#include <zlink/framework/detail/sha256.hpp>

#include <array>
#include <charconv>
#include <set>
#include <tuple>

namespace zlink::framework::runtime::relocation_completion
{
namespace
{
namespace wire = protocol::generated::detail;
namespace authority = authority_aggregate_codec;

using json_t = authority::json_t;

constexpr std::size_t chunk_bytes = protocol::relocationChunkBytes;
constexpr std::array<std::string_view, 4> delivery_names{"pending", "terminalReceived",
                                                         "alreadyTerminal", "sourceLeaseExpired"};

[[noreturn]] void data_lost (const char *message)
{
    throw framework_exception_t (framework_error_kind_t::data_lost, message);
}

void validate_entry (const entry_t &entry)
{
    const auto &key = entry.object_key;
    const auto &source = entry.source;
    if (!authority_key_codec_detail::authority_key_kind (key) || !detail::is_valid_utf8 (key)
        || source.owner_id.empty () || !detail::is_valid_utf8 (source.owner_id)
        || source.node_rid.empty () || source.node_rid.size () % 2 != 0
        || !authority::lower_hex (source.node_rid) || source.lease_generation == 0
        || source.node_generation == 0 || entry.operation_id.size () != detail::uuid_hex_length
        || !authority::lower_hex (entry.operation_id)
        || entry.operation_id.find_first_not_of ('0') == std::string::npos
        || entry.reply_route_id == 0
        || static_cast<std::size_t> (entry.delivery) >= delivery_names.size ())
        data_lost ("Invalid relocation completion entry");
}

void validate (const record_t &record)
{
    if (!detail::is_lowercase_uuid_text (record.relocation_id))
        data_lost ("Invalid relocation completion identity");
    std::set<std::tuple<const source_t &, std::string_view>> seen;
    std::string_view previous;
    for (const auto &entry : record.entries) {
        validate_entry (entry);
        if (entry.object_key < previous || !seen.emplace (entry.source, entry.operation_id).second)
            data_lost ("Relocation completion entries are unordered or duplicated");
        previous = entry.object_key;
    }
}

json_t entry_json (const entry_t &entry, delivery_t delivery, std::string reply)
{
    // The field names are in RFC 8785 order.
    return {{"delivery", delivery_names[static_cast<std::size_t> (delivery)]},
            {"objectKey", entry.object_key},
            {"operationId", entry.operation_id},
            {"reply", std::move (reply)},
            {"replyRouteId", std::to_string (entry.reply_route_id)},
            {"source",
             {{"leaseGeneration", std::to_string (entry.source.lease_generation)},
              {"nodeGeneration", std::to_string (entry.source.node_generation)},
              {"nodeRid", entry.source.node_rid},
              {"ownerId", entry.source.owner_id}}}};
}

std::string record_suffix (const std::string &id)
{
    return "],\"formatVersion\":\"1\",\"relocationId\":\"" + id + "\"}";
}

constexpr std::string_view record_prefix = "{\"entries\":[";

entry_t decode_entry (const json_t &item)
{
    const auto &source = item.at ("source");
    const auto delivery = std::find (delivery_names.begin (), delivery_names.end (),
                                     authority::text (item, "delivery"));
    return {authority::text (item, "objectKey"),
            {authority::text (source, "ownerId"), authority::number (source, "leaseGeneration"),
             authority::text (source, "nodeRid"), authority::number (source, "nodeGeneration")},
            authority::text (item, "operationId"),
            authority::number (item, "replyRouteId"),
            base64_decode (item.at ("reply").get_ref<const std::string &> ()),
            static_cast<delivery_t> (delivery - delivery_names.begin ())};
}

std::span<const std::uint8_t> octets (std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const std::uint8_t *> (bytes.data ()), bytes.size ()};
}

std::vector<std::byte> blob_bytes (const wire::result_t<std::vector<std::uint8_t>> &encoded)
{
    if (!encoded)
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "Cannot encode a relocation completion blob");
    const auto bytes = std::as_bytes (std::span (encoded.value));
    return {bytes.begin (), bytes.end ()};
}

const authority::json_t &relocation_state (const authority_snapshot_t &snapshot)
{
    if (!snapshot.aggregate || !snapshot.aggregate->relocation)
        data_lost ("Relocation completion authority is missing");
    return snapshot.aggregate->encoded.at ("relocation");
}

const authority::json_t &progress (const authority::json_t &state)
{
    const auto completion = state.find ("completion");
    if (completion == state.end ())
        data_lost ("Relocation completion counts are missing");
    // Authority decoding has already validated the structure and count bounds.
    return *completion;
}

}

std::size_t record_t::pending_deliveries () const
{
    return std::count_if (entries.begin (), entries.end (),
                          [] (const auto &entry) { return entry.delivery == delivery_t::pending; });
}

std::string encode (const record_t &record)
{
    validate (record);
    std::string bytes (record_prefix);
    bool first = true;
    for (const auto &entry : record.entries) {
        if (!std::exchange (first, false))
            bytes += ',';
        bytes += entry_json (entry, entry.delivery, base64_encode (entry.reply)).dump ();
    }
    bytes += record_suffix (record.relocation_id);
    return bytes;
}

record_t decode (std::string_view bytes)
{
    try {
        auto json = json_t::parse (bytes);
        const auto &entries = json.at ("entries");
        if (authority::text (json, "formatVersion") != "1" || !entries.is_array ())
            data_lost ("Invalid relocation completion format");
        record_t record{authority::text (json, "relocationId"), {}};
        record.entries.reserve (entries.size ());
        for (const auto &item : entries)
            record.entries.push_back (decode_entry (item));
        if (encode (record) != bytes)
            data_lost ("Relocation completion JSON is not canonical");
        return record;
    }
    catch (const json_t::exception &) {
        data_lost ("Invalid relocation completion JSON");
    }
    catch (const std::invalid_argument &) {
        data_lost ("Invalid relocation completion field");
    }
}

void require_fits (const record_t &record,
                   std::span<const entry_t> remaining_failures,
                   std::uint64_t limit)
{
    validate (record);
    std::uint64_t size = record_prefix.size () + record_suffix (record.relocation_id).size ();
    bool first = true;
    const auto account = [&] (const entry_t &entry) {
        const auto reply_size = static_cast<std::uint64_t> (entry.reply.size ());
        const auto groups = reply_size / 3 + (reply_size % 3 != 0);
        const auto overhead =
          entry_json (entry, delivery_t::source_lease_expired, {}).dump ().size ()
          + !std::exchange (first, false);
        if (overhead > limit - size || groups > (limit - size - overhead) / 4)
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "Final relocation completion record exceeds the blob limit");
        size += overhead + groups * 4;
    };
    if (size > limit)
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "Final relocation completion record exceeds the blob limit");
    for (const auto &entry : record.entries)
        account (entry);
    for (const auto &entry : remaining_failures) {
        validate_entry (entry);
        account (entry);
    }
}

task_t<std::vector<std::byte>> store_t::read_blob (std::string reference,
                                                   std::stop_token cancellation)
{
    auto result = co_await _blobs.get_relocation (std::move (reference), cancellation);
    auto *found = std::get_if<relocation_found_t> (&result);
    if (!found)
        data_lost ("Relocation completion blob is missing");
    co_return std::move (found->payload);
}

task_t<record_t> store_t::read (authority_snapshot_t snapshot, std::stop_token cancellation)
{
    const auto &state = relocation_state (snapshot);
    const auto &completion = progress (state);
    const auto &id = snapshot.aggregate->relocation->relocation_id;
    if (completion.at ("reference").is_null ())
        co_return record_t{id, {}};

    const auto root = co_await read_blob (authority::text (completion, "reference"), cancellation);
    const auto manifest = wire::decode_durable_relocation_manifest_v1 (octets (root));
    if (!manifest
        || detail::encode_hex (
             std::span<const std::uint8_t> (manifest.value.body.inventoryDigestSha256.value))
             != authority::text (state, "sourceInventorySha256"))
        data_lost ("Invalid relocation completion manifest");

    const auto &body = manifest.value.body;
    std::string bytes;
    detail::crc32c_accumulator_t checksum;
    std::size_t order = 0;
    for (const auto &entry : body.chunks.items) {
        const auto stored = co_await read_blob (entry.reference.value, cancellation);
        const auto chunk = wire::decode_durable_relocation_data_chunk_v1 (octets (stored));
        if (!chunk || entry.order.value != order || chunk.value.body.order.value != order)
            data_lost ("Invalid relocation completion chunk order");
        const std::span<const std::uint8_t> data (chunk.value.body.data.value);
        if (entry.length.value != data.size ()
            || data.size () > body.totalLength.value - bytes.size ()
            || detail::crc32c (data) != entry.checksumCrc32c.value)
            data_lost ("Invalid relocation completion chunk");
        ++order;
        checksum.update (data);
        bytes.append (reinterpret_cast<const char *> (data.data ()), data.size ());
    }
    if (bytes.size () != body.totalLength.value
        || checksum.value () != body.totalChecksumCrc32c.value
        || detail::sha256_hex (bytes) != authority::text (completion, "sha256"))
        data_lost ("Relocation completion payload checksum differs");

    auto record = decode (bytes);
    if (record.relocation_id != id
        || record.entries.size () != authority::number (completion, "completedResults")
        || record.pending_deliveries () != authority::number (completion, "pendingDeliveries"))
        data_lost ("Relocation completion payload disagrees with authority");
    co_return record;
}

task_t<authority_compare_exchange_result_t>
store_t::publish (authority_key_t key,
                  authority_snapshot_t expected,
                  record_t record,
                  std::span<const entry_t> remaining_failures,
                  std::stop_token cancellation)
{
    auto state = relocation_state (expected);
    const auto &previous = progress (state);
    const auto accepted = authority::number (previous, "acceptedRequests");
    if (record.relocation_id != expected.aggregate->relocation->relocation_id
        || record.entries.empty () || record.entries.size () > accepted
        || remaining_failures.size () != accepted - record.entries.size ()
        || record.entries.size () < authority::number (previous, "completedResults"))
        data_lost ("Relocation completion identity or result count differs");
    require_fits (record, remaining_failures);

    const auto bytes = encode (record);
    const auto payload = octets (std::as_bytes (std::span (bytes)));
    wire::relocation_manifest_v1_t manifest;
    manifest.logicalFormatVersion.value = 1;
    manifest.totalLength.value = payload.size ();
    manifest.totalChecksumCrc32c.value = detail::crc32c (payload);
    const auto &inventory = authority::text (state, "sourceInventorySha256");
    for (std::size_t pos = 0; pos < inventory.size (); pos += 2) {
        unsigned byte = 0;
        std::from_chars (inventory.data () + pos, inventory.data () + pos + 2, byte, 16);
        manifest.inventoryDigestSha256.value.push_back (static_cast<std::uint8_t> (byte));
    }
    for (std::size_t offset = 0; offset < payload.size (); offset += chunk_bytes) {
        const auto data =
          payload.subspan (offset, std::min (chunk_bytes, payload.size () - offset));
        const auto order = static_cast<std::uint32_t> (manifest.chunks.items.size ());
        const auto checksum = detail::crc32c (data);
        auto chunk = blob_bytes (wire::encode_durable_relocation_data_chunk_v1 (
          {{wire::u32_t{order}, wire::durable_blob_t{{data.begin (), data.end ()}}}}));
        auto reference = co_await _blobs.put_verified_blob (
          std::move (chunk), relocation_repository_t::default_retention, cancellation);
        manifest.chunks.items.push_back (
          {{order}, {std::move (reference)}, {data.size ()}, {checksum}});
    }
    auto root = blob_bytes (wire::encode_durable_relocation_manifest_v1 ({std::move (manifest)}));
    const auto reference = co_await _blobs.put_verified_blob (
      std::move (root), relocation_repository_t::default_retention, cancellation);

    state["completion"] = {{"acceptedRequests", std::to_string (accepted)},
                           {"completedResults", std::to_string (record.entries.size ())},
                           {"pendingDeliveries", std::to_string (record.pending_deliveries ())},
                           {"reference", reference},
                           {"sha256", detail::sha256_hex (bytes)}};
    authority_completion_put_t mutation{authority::replace (expected.aggregate, std::move (state))};
    auto written = co_await await_result (_authorities.compare_exchange_authority (
      key, expected.store_version, std::move (mutation), cancellation));
    const auto *error = written.error ();
    const bool needs_confirmation =
      written ? std::holds_alternative<authority_conflict_t> (written.value ())
              : error && detail::is_transient_error (error->kind ());
    if (!needs_confirmation || cancellation.stop_requested ())
        co_return written.value ();

    auto current = co_await _authorities.read_authority (std::move (key), cancellation);
    auto *observed = std::get_if<authority_snapshot_t> (&current);
    if (observed && observed->same_incarnation (expected)
        && observed->owner.lease_generation == expected.owner.lease_generation
        && observed->aggregate && observed->aggregate->relocation
        && observed->aggregate->relocation->relocation_id == record.relocation_id) {
        const auto &state = relocation_state (*observed);
        if (const auto completion = state.find ("completion");
            completion != state.end () && completion->at ("reference") == reference) {
            (void) co_await read (*observed, cancellation);
            co_return authority_compare_exchange_result_t{
              authority_stored_t{std::move (*observed)}};
        }
    }
    co_return written.value ();
}

} // namespace zlink::framework::runtime::relocation_completion
