/* SPDX-License-Identifier: Elastic-2.0 */

#include "provider_location_repository.hpp"

#include <charconv>
#include <limits>
#include <span>

namespace zlink::framework::runtime
{
class provider_location_repository_t::store_format_t
{
  public:
    store_format_t (location_store_t &store, std::stop_token cancellation) :
        _store (store), _cancellation (cancellation)
    {
    }

    task_t<std::uint64_t> convert ()
    {
        auto completed = co_await read_completed_floor ();
        if (!completed)
            completed = co_await settle_legacy_aggregates ();
        if (completed)
            co_return *completed;

        const auto floor = co_await ensure_floor ();
        co_await move_capacity ();
        const auto marker = co_await put_missing (format_key, to_bytes (converted));
        validate_marker (marker);
        co_return floor;
    }

  private:
    inline static const store_key_t format_key{"zlink:v12:format"};
    static constexpr std::string_view converted = "converted";
    static constexpr std::string_view legacy_capacity = "zlink:v11:capacity:";

    [[noreturn]] static void invalid_format (std::string message)
    {
        throw framework_exception_t (framework_error_kind_t::protocol_error, std::move (message));
    }

    void check_cancelled () const
    {
        if (_cancellation.stop_requested ())
            std::rethrow_exception (
              detail::make_cancellation_exception ("Location Store conversion was cancelled"));
    }

    task_t<store_read_result_t> read (store_key_t key)
    {
        check_cancelled ();
        return _store.read (std::move (key));
    }

    task_t<store_scan_result_t> scan (std::string scan_prefix,
                                      std::optional<store_scan_cursor_t> cursor)
    {
        check_cancelled ();
        return _store.scan (
          {std::move (scan_prefix), std::move (cursor), location_page_item_limit});
    }

    static std::uint64_t migration_generation (const std::vector<std::byte> &bytes)
    {
        const auto value = parse_location_counter (to_string (bytes));
        if (!value || *value == 0)
            invalid_format ("Invalid Location Store generation floor input");
        return static_cast<std::uint64_t> (*value);
    }

    static std::uint64_t migration_generation (const store_read_result_t &row)
    {
        const auto *found = std::get_if<store_found_t> (&row);
        return found ? migration_generation (found->value.bytes) : 1;
    }

    static void validate_marker (const std::vector<std::byte> &bytes)
    {
        if (to_string (bytes) != converted)
            invalid_format ("The Location Store format is unrecognized");
    }

    task_t<std::optional<std::uint64_t>> read_completed_floor ()
    {
        const auto marker = co_await read (format_key);
        const auto *found = std::get_if<store_found_t> (&marker);
        if (!found)
            co_return std::nullopt;
        validate_marker (found->value.bytes);
        const auto floor = co_await read (generation_floor_key);
        const auto *stored = std::get_if<store_found_t> (&floor);
        if (!stored)
            invalid_format ("The converted Location Store has no generation floor");
        co_return migration_generation (stored->value.bytes);
    }

    enum class legacy_state_t
    {
        staging,
        prepared,
        committed,
        aborted
    };
    enum class legacy_writer_t
    {
        cpp,
        java,
        dotnet,
        node
    };

    struct legacy_aggregate_t
    {
        legacy_writer_t writer;
        legacy_state_t state;
        std::string identity;
        std::string id;
        std::uint64_t generation{};
        std::string mesh;
        std::string rid_hex;
        std::string capacity_rid;
        std::uint64_t lifecycle{};
        location_owner_token_t owner;
        placement_capacity_bundle_t bundle;
        json_t header;
    };

    static const char *legacy_state_field (const json_t &record)
    {
        const auto state = record.find ("state");
        return state != record.end () && !state->is_null () ? "state" : "status";
    }

    static legacy_state_t legacy_state (const json_t &state)
    {
        if (state == "staging" || state == aggregate_status::preparing || state == 0)
            return legacy_state_t::staging;
        if (state == aggregate_status::prepared || state == aggregate_status::committing
            || state == 1)
            return legacy_state_t::prepared;
        if (state == aggregate_status::committed || state == 2)
            return legacy_state_t::committed;
        if (state == aggregate_status::aborted || state == 3)
            return legacy_state_t::aborted;
        invalid_format ("Invalid v11 aggregate state");
    }

    static std::uint64_t legacy_generation (const json_t &field,
                                            std::uint64_t limit = max_generation)
    {
        const auto &value = field.is_object () ? field.at ("$bigint") : field;
        const auto decimal = value.is_string () ? value.get<std::string> () : value.dump ();
        std::uint64_t number{};
        const auto end = decimal.data () + decimal.size ();
        const auto [parsed, error] = std::from_chars (decimal.data (), end, number);
        if (error != std::errc{} || parsed != end || decimal.front () == '0' || number > limit)
            invalid_format ("Invalid v11 aggregate generation");
        return number;
    }

    static std::uint32_t legacy_count (const json_t &field)
    {
        const auto number = parse_location_counter (field.dump ());
        if (!number || *number > std::numeric_limits<std::uint32_t>::max ())
            invalid_format ("Invalid v11 aggregate capacity count");
        return static_cast<std::uint32_t> (*number);
    }

    static std::string dashed_uuid (std::string hex_id)
    {
        for (const auto offset : {20u, 16u, 12u, 8u})
            hex_id.insert (offset, "-");
        return hex_id;
    }

    static std::string foreign_rid_display (const zlink::routing_id_t &rid)
    {
        auto display = rid.to_string ();
        if (display != raw_rid (rid))
            return display;

        // Java and .NET also reject UTF-8 C1 controls in their display form.
        const auto is_c1 = [] (unsigned char first, unsigned char second) {
            return first == 0xc2 && second >= 0x80 && second <= 0x9f;
        };
        if (detail::is_valid_non_nul_utf8 (display)
            && std::adjacent_find (display.begin (), display.end (), is_c1) == display.end ())
            return display;
        const auto rid_hex = rid.to_hex ();
        if (rid.size () == 4) {
            std::uint32_t value = 0;
            std::from_chars (rid_hex.data (), rid_hex.data () + rid_hex.size (), value, 16);
            return std::to_string (value);
        }
        if (rid.size () == 16)
            return dashed_uuid (rid_hex);
        return "hex:" + rid_hex;
    }

    static std::string compact_uuid (std::string_view id)
    {
        if (id.size () != 32
            && (id.size () != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-'
                || id[23] != '-'))
            invalid_format ("Invalid v11 aggregate UUID");
        std::string compact;
        for (const auto ch : id) {
            if (ch != '-')
                compact += ch;
        }
        return hex (unhex_array<16> (compact));
    }

    static placement_object_kind_t legacy_spot_kind (const json_t &kind)
    {
        if (kind == 2 || kind == "user_spot"
            || kind == object_kind3_name (placement_object_kind_t::user_spot))
            return placement_object_kind_t::user_spot;
        if (kind == 3 || kind == "instance_spot"
            || kind == object_kind3_name (placement_object_kind_t::instance_spot))
            return placement_object_kind_t::instance_spot;
        invalid_format ("Invalid v11 aggregate spot kind");
    }

    static placement_capacity_bundle_t legacy_bundle (const json_t &bundle)
    {
        placement_capacity_bundle_t result{legacy_count (bundle.at ("actors")),
                                           legacy_count (bundle.at ("spots")), std::nullopt};
        if (const auto spot = bundle.find ("spotType"); spot != bundle.end () && !spot->is_null ())
            result.spot_type = spot_type_capacity_delta_t{
              legacy_spot_kind (spot->at ("objectKind")),
              spot->at ("stableType").get<std::string> (), legacy_count (spot->at ("count"))};
        return result;
    }

    class java_aggregate_reader_t
    {
      public:
        explicit java_aggregate_reader_t (std::span<const std::byte> bytes) : _bytes (bytes) {}

        std::span<const std::byte> take (std::uint64_t size)
        {
            if (size > _bytes.size ())
                invalid_format ("Truncated Java v11 aggregate");
            const auto head = _bytes.first (static_cast<std::size_t> (size));
            _bytes = _bytes.subspan (head.size ());
            return head;
        }

        std::uint64_t number (std::size_t width)
        {
            std::uint64_t result = 0;
            for (auto octet : take (width))
                result = (result << 8) | std::to_integer<unsigned> (octet);
            return result;
        }

        std::string text ()
        {
            // DataOutputStream.writeUTF uses modified UTF-8 over UTF-16 code units.
            const auto input = take (number (2));
            std::u16string utf16;
            for (std::size_t i = 0; i < input.size ();) {
                const auto first = std::to_integer<unsigned> (input[i++]);
                unsigned value = first;
                if (first >= 0x80) {
                    const auto width = first < 0xe0 ? 2u : 3u;
                    if (first < 0xc0 || first >= 0xf0 || input.size () - i < width - 1)
                        invalid_format ("Invalid Java v11 aggregate text");
                    value = first & (width == 2 ? 0x1f : 0x0f);
                    for (unsigned j = 1; j < width; ++j) {
                        const auto next = std::to_integer<unsigned> (input[i++]);
                        if ((next & 0xc0) != 0x80)
                            invalid_format ("Invalid Java v11 aggregate text");
                        value = (value << 6) | (next & 0x3f);
                    }
                }
                utf16 += static_cast<char16_t> (value);
            }
            std::string utf8;
            for (std::size_t i = 0; i < utf16.size (); ++i) {
                unsigned cp = utf16[i];
                if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < utf16.size () && utf16[i + 1] >= 0xdc00
                    && utf16[i + 1] <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (utf16[++i] - 0xdc00);
                } else if (cp >= 0xd800 && cp <= 0xdfff) {
                    invalid_format ("Invalid Java v11 aggregate surrogate");
                }
                if (cp < 0x80) {
                    utf8 += static_cast<char> (cp);
                } else {
                    const unsigned width = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                    utf8 += static_cast<char> ((0xff << (8 - width)) | (cp >> (6 * (width - 1))));
                    for (unsigned shift = 6 * (width - 1); shift != 0;) {
                        shift -= 6;
                        utf8 += static_cast<char> (0x80 | ((cp >> shift) & 0x3f));
                    }
                }
            }
            return utf8;
        }

        bool empty () const { return _bytes.empty (); }

      private:
        std::span<const std::byte> _bytes;
    };

    static legacy_aggregate_t decode_java (const std::vector<std::byte> &bytes)
    {
        java_aggregate_reader_t reader (bytes);
        legacy_aggregate_t root{};
        root.writer = legacy_writer_t::java;
        const auto state = reader.number (1);
        if (state > 2)
            invalid_format ("Invalid Java v11 aggregate state");
        root.state = legacy_state (state);
        root.id = dashed_uuid (detail::encode_hex (reader.take (16)));
        root.generation = legacy_generation (reader.number (8));
        root.identity = root.id + ":" + std::to_string (root.generation);
        root.mesh = reader.text ();
        const auto rid = zlink::routing_id_t::from_hex (reader.text ());
        root.rid_hex = rid.to_hex ();
        root.capacity_rid = foreign_rid_display (rid);
        root.lifecycle = reader.number (8);
        root.owner.owner_id = reader.text ();
        root.owner.lease_generation =
          static_cast<std::int64_t> (legacy_generation (reader.number (8)));
        (void) reader.take (reader.number (4)); // Inventory digest.
        root.bundle.actor_slots = legacy_count (reader.number (4));
        root.bundle.spot_slots = legacy_count (reader.number (4));
        const auto has_spot = reader.number (1);
        if (has_spot > 1)
            invalid_format ("Invalid Java v11 aggregate capacity vector");
        if (has_spot) {
            const auto kind = legacy_spot_kind (reader.number (4) + 1); // Java enum ordinal.
            auto type = reader.text ();
            root.bundle.spot_type =
              spot_type_capacity_delta_t{kind, std::move (type), legacy_count (reader.number (4))};
        }
        if (legacy_count (reader.number (4)) == 0 || reader.number (4) != 32)
            invalid_format ("Invalid Java v11 aggregate inventory header");
        (void) reader.take (32);
        if (!reader.empty ())
            invalid_format ("Trailing bytes in Java v11 aggregate");
        return root;
    }

    static legacy_aggregate_t decode_json_aggregate (const json_t &record, std::string_view suffix)
    {
        legacy_aggregate_t root{};
        root.state = legacy_state (record.at (legacy_state_field (record)));
        if (record.contains ("header")) {
            root.writer = legacy_writer_t::dotnet;
            root.header = record.at ("header");
            root.id = compact_uuid (root.header.at ("aggregateId").get<std::string> ());
            root.generation = legacy_generation (root.header.at ("aggregateGeneration"));
            root.identity = root.id + ":" + std::to_string (root.generation);
        } else if (record.contains ("targetDescriptor")) {
            root.writer = legacy_writer_t::node;
            const auto separator = suffix.rfind (':');
            if (separator == std::string_view::npos)
                invalid_format ("Invalid Node v11 aggregate key");
            root.id = decode_component (suffix.substr (0, separator));
            root.generation = legacy_generation (std::string (suffix.substr (separator + 1)));
            root.identity =
              encode_legacy_component (root.id) + ":" + std::to_string (root.generation);
        } else {
            root.writer = legacy_writer_t::cpp;
            root.id = compact_uuid (record.at ("aggregateId").get<std::string> ());
            root.generation = legacy_generation (record.at ("aggregateGeneration"));
            root.identity = root.id;
        }

        const auto &header = root.writer == legacy_writer_t::dotnet ? root.header : record;
        const bool cpp = root.writer == legacy_writer_t::cpp;
        const auto &descriptor = cpp ? header : header.at ("targetDescriptor");
        root.mesh = descriptor.at (cpp ? "targetMeshName" : "meshName").get<std::string> ();
        if (cpp) {
            // The v11 C++ writer stored the display RID used in the capacity key.
            root.capacity_rid = descriptor.at ("targetNodeRid").get<std::string> ();
        } else if (root.writer == legacy_writer_t::node) {
            const auto &rid = descriptor.at ("rid");
            if (rid.is_string ()) {
                root.capacity_rid = rid.get<std::string> ();
                root.rid_hex = zlink::routing_id_t::from (root.capacity_rid).to_hex ();
            } else {
                root.capacity_rid = rid.at ("text").get<std::string> ();
                root.rid_hex =
                  zlink::routing_id_t::from_hex (rid.at ("hex").get<std::string> ()).to_hex ();
            }
        } else {
            const auto rid =
              zlink::routing_id_t::from_hex (descriptor.at ("routingIdHex").get<std::string> ());
            root.capacity_rid = foreign_rid_display (rid);
            root.rid_hex = rid.to_hex ();
        }
        root.lifecycle = legacy_generation (
          header.at (cpp ? "targetLifecycleGeneration" : "targetDescriptorLifecycleGeneration"),
          std::numeric_limits<std::uint64_t>::max ());
        const auto &owner = header.at ("targetOwner");
        root.owner = {owner.at ("ownerId").get<std::string> (),
                      static_cast<std::int64_t> (legacy_generation (owner.at ("leaseGeneration")))};
        root.bundle = legacy_bundle (header.at (cpp ? "capacityBundle" : "capacity"));
        return root;
    }

    static legacy_aggregate_t decode_legacy_aggregate (const store_scan_item_t &item)
    {
        try {
            const auto suffix = std::string_view (item.key.value).substr (aggregate_prefix.size ());
            const auto &bytes = item.value.bytes;
            auto root = !bytes.empty () && std::to_integer<unsigned> (bytes.front ()) <= 3
                          ? decode_java (bytes)
                          : decode_json_aggregate (parse_json (bytes), suffix);
            if (root.identity != suffix || root.id.empty () || root.capacity_rid.empty ()
                || root.lifecycle == 0 || root.mesh.empty ()
                || root.mesh.find ('\0') != std::string::npos || root.owner.owner_id.empty ()
                || root.owner.owner_id.find ('\0') != std::string::npos
                || root.owner.lease_generation <= 0)
                invalid_format ("Invalid v11 aggregate identity or target");
            return root;
        }
        catch (const json_t::exception &) {
            invalid_format ("Invalid v11 aggregate: " + item.key.value);
        }
        catch (const std::invalid_argument &) {
            invalid_format ("Invalid v11 aggregate: " + item.key.value);
        }
        catch (const std::out_of_range &) {
            invalid_format ("Invalid v11 aggregate: " + item.key.value);
        }
    }

    static bool legacy_inline_child (std::string_view key)
    {
        const auto suffix = key.substr (aggregate_prefix.size ());
        const auto id_end = suffix.find (':');
        const auto generation_end =
          id_end == std::string_view::npos ? id_end : suffix.find (':', id_end + 1);
        return generation_end != std::string_view::npos
               && (suffix.substr (generation_end).starts_with (":participant:")
                   || suffix.substr (generation_end).starts_with (":inventory:"));
    }

    template <typename Rows, typename Collect>
    task_t<Rows> scan_legacy_rows (std::string prefix, Collect collect)
    {
        Rows rows;
        std::optional<store_scan_cursor_t> cursor;
        for (;;) {
            auto scanned = co_await scan (prefix, cursor);
            auto *page = std::get_if<store_scan_page_t> (&scanned);
            if (!page) {
                rows.clear ();
                cursor.reset ();
                continue;
            }
            for (auto &item : page->items)
                collect (rows, std::move (item));
            cursor = std::move (page->next_cursor);
            if (!cursor)
                co_return rows;
        }
    }

    task_t<std::vector<store_scan_item_t>> scan_legacy_rows (std::string prefix)
    {
        return scan_legacy_rows<std::vector<store_scan_item_t>> (
          std::move (prefix), [] (auto &rows, auto item) { rows.push_back (std::move (item)); });
    }

    task_t<store_condition_t> expired_target_condition (const legacy_aggregate_t &root)
    {
        const auto key = key_owner (root.owner.owner_id);
        const auto current = co_await read (key);
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            if (!found->value.expires_at)
                invalid_format ("Invalid v11 target owner lease expiry: " + root.identity);
            const auto lease = decode_owner_lease (*found);
            if (lease.token.owner_id != root.owner.owner_id)
                invalid_format ("Invalid v11 target owner lease: " + root.identity);
            if (owner_lease_is_live (lease, root.owner))
                invalid_format ("Live v11 aggregate target owner: " + root.identity);
        }
        co_return condition_for (key, current);
    }

    static std::string encode_legacy_component (std::string_view value)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        for (const unsigned char byte : value) {
            if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9')
                || std::string_view ("-_.!~*'()").find (byte) != std::string_view::npos) {
                encoded += static_cast<char> (byte);
            } else {
                encoded += '%';
                encoded += digits[byte >> 4];
                encoded += digits[byte & 0xf];
            }
        }
        return encoded;
    }

    static bool return_pending (json_t &record, const placement_capacity_bundle_t &bundle)
    {
        std::map<std::string, std::uint32_t> remaining;
        if (bundle.actor_slots != 0)
            remaining.emplace ("actors", bundle.actor_slots);
        if (bundle.spot_slots != 0)
            remaining.emplace ("spots", bundle.spot_slots);
        if (bundle.spot_type && bundle.spot_type->slots != 0) {
            const auto &spot = *bundle.spot_type;
            remaining.emplace (capacity_type_dimension (spot.object_kind, spot.stable_type),
                               spot.slots);
        }
        bool sufficient = true;
        for_each_capacity_count (
          record, [&] (std::string_view name, std::string_view state, json_t &field) {
              if (state != "pending")
                  return;
              const auto count = remaining.find (std::string (name));
              if (count == remaining.end ())
                  return;
              const auto previous = read_capacity_count (field);
              if (count->second > previous)
                  sufficient = false;
              else
                  field = previous - count->second;
              remaining.erase (count);
          });
        return sufficient && remaining.empty ();
    }

    task_t<bool> append_legacy_release (const legacy_aggregate_t &root,
                                        store_write_request_t &batch)
    {
        const auto &bundle = root.bundle;
        if (bundle.actor_slots == 0 && bundle.spot_slots == 0
            && (!bundle.spot_type || bundle.spot_type->slots == 0))
            co_return true;
        const store_key_t key{std::string (legacy_capacity) + encode_legacy_component (root.mesh)
                              + ":" + encode_legacy_component (root.capacity_rid)};
        const auto current = co_await read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            co_return false;
        try {
            auto record = parse_json (found->value.bytes);
            if (!return_pending (record, bundle))
                co_return false;
            batch.conditions.push_back (version_condition (key, found->value.version));
            batch.mutations.push_back (store_put_t{key, to_bytes (record.dump ()), std::nullopt});
        }
        catch (const json_t::exception &) {
            invalid_format ("Invalid v11 aggregate target capacity: " + key.value);
        }
        co_return true;
    }

    task_t<bool> settle_batch (const store_write_request_t &batch)
    {
        if (!batch_fits (batch))
            invalid_format ("v11 aggregate settlement exceeds Store batch limits");
        check_cancelled ();
        const auto result = co_await await_result (_store.write (batch));
        co_return applied (result);
    }

    task_t<void> settle_child (const store_scan_item_t &root,
                               const store_condition_t &lease,
                               const store_scan_item_t &child,
                               store_mutation_t mutation)
    {
        const store_write_request_t batch{{version_condition (root.key, root.value.version), lease,
                                           version_condition (child.key, child.value.version)},
                                          {std::move (mutation)}};
        auto current_child = co_await read (child.key);
        if (const auto *found = std::get_if<store_found_t> (&current_child);
            found && found->value.version.value == child.value.version.value) {
            if (co_await settle_batch (batch))
                co_return;
            current_child = co_await read (child.key);
        }
        const auto *found = std::get_if<store_found_t> (&current_child);
        const auto *put = std::get_if<store_put_t> (&batch.mutations.front ());
        if ((put && found && found->value.bytes == put->bytes) || (!put && !found))
            co_return;
        const auto current_root = co_await read (root.key);
        if (!std::holds_alternative<store_missing_t> (current_root))
            invalid_format ("Could not settle v11 aggregate child: " + child.key.value);
    }

    task_t<void> delete_legacy_children (const store_scan_item_t &root,
                                         const store_condition_t &lease,
                                         std::string prefix)
    {
        for (const auto &child : co_await scan_legacy_rows (std::move (prefix)))
            co_await settle_child (root, lease, child, store_delete_t{child.key});
    }

    using legacy_fence_t = std::pair<std::string, std::uint64_t>;
    using legacy_participants_t = std::map<legacy_fence_t, std::vector<store_scan_item_t>>;

    static legacy_fence_t legacy_fence (const legacy_aggregate_t &aggregate)
    {
        return {aggregate.identity, aggregate.generation};
    }

    task_t<legacy_participants_t>
    scan_legacy_participants (const std::map<legacy_fence_t, legacy_writer_t> &writers)
    {
        const auto cpp_count =
          std::count_if (writers.begin (), writers.end (),
                         [] (const auto &entry) { return entry.second == legacy_writer_t::cpp; });
        legacy_participants_t participants;
        if (cpp_count != 0) {
            participants = co_await scan_legacy_rows<legacy_participants_t> (
              std::string (prefix) + "aggregate-lock:",
              [&] (auto &by_fence, store_scan_item_t item) {
                  const auto lock = decode_aggregate_lock (item.value.bytes);
                  if (!lock)
                      invalid_format ("Invalid C++ v11 aggregate lock: " + item.key.value);
                  legacy_fence_t fence{hex (lock->aggregate_id.value), lock->aggregate_generation};
                  if (writers.contains (fence))
                      by_fence[std::move (fence)].push_back (std::move (item));
              });
        }
        if (static_cast<std::size_t> (cpp_count) == writers.size ())
            co_return participants;

        auto embedded = co_await scan_legacy_rows<legacy_participants_t> (
          prefix_authority (), [&] (auto &by_fence, store_scan_item_t item) {
              const auto authority = parse_json (item.value.bytes);
              const auto dotnet = authority.find ("aggregateFence");
              const auto common = authority.find ("aggregate");
              const bool dotnet_marker = dotnet != authority.end () && !dotnet->is_null ();
              const auto marker = dotnet_marker ? dotnet : common;
              if (marker == authority.end () || !marker->contains ("aggregateId"))
                  return;
              const auto id = marker->at ("aggregateId").get<std::string> ();
              const auto generation = legacy_generation (marker->at ("aggregateGeneration"));
              const auto identity =
                (dotnet_marker ? compact_uuid (id) : encode_legacy_component (id)) + ":"
                + std::to_string (generation);
              legacy_fence_t fence{identity, generation};
              const auto writer = writers.find (fence);
              if (writer != writers.end () && writer->second != legacy_writer_t::cpp
                  && (writer->second == legacy_writer_t::dotnet) == dotnet_marker)
                  by_fence[std::move (fence)].push_back (std::move (item));
          });
        participants.merge (embedded);
        co_return participants;
    }

    task_t<void> settle_cpp_lock (const legacy_aggregate_t &aggregate,
                                  const store_scan_item_t &root,
                                  const store_condition_t &lease,
                                  const store_scan_item_t &item)
    {
        const auto lock = decode_aggregate_lock (item.value.bytes);
        if (!lock || item.key.value != key_aggregate_lock (lock->authority_key).value)
            invalid_format ("Invalid C++ v11 aggregate lock: " + item.key.value);
        if (aggregate.state != legacy_state_t::committed
            && lock->status == aggregate_status::committing) {
            const auto current_lock = co_await read (item.key);
            const auto *held = std::get_if<store_found_t> (&current_lock);
            if (!held)
                co_return;
            if (held->value.version.value != item.value.version.value)
                invalid_format ("C++ v11 aggregate participant lock changed: " + item.key.value);
            if (!lock->page_index || !lock->entry_index)
                invalid_format ("Missing C++ v11 aggregate commit entry");
            const auto page =
              co_await read (key_aggregate_commit_page (lock->aggregate_id, *lock->page_index));
            const auto *found = std::get_if<store_found_t> (&page);
            const auto entries =
              found ? decode_aggregate_commit_page (found->value.bytes, *lock->page_index)
                    : std::nullopt;
            if (!entries || *lock->entry_index >= entries->size ()) {
                const auto current = co_await read (item.key);
                if (std::holds_alternative<store_missing_t> (current))
                    co_return;
                invalid_format ("Missing C++ v11 aggregate commit page");
            }
            const auto &entry = (*entries)[*lock->entry_index];
            const auto authority_key = key_authority (lock->authority_key);
            const auto authority = co_await read (authority_key);
            const auto *stored = std::get_if<store_found_t> (&authority);
            if (entry.authority_key != lock->authority_key || !stored
                || (stored->value.bytes != entry.before && stored->value.bytes != entry.after)) {
                const auto current = co_await read (item.key);
                if (std::holds_alternative<store_missing_t> (current))
                    co_return;
                invalid_format ("Invalid C++ v11 aggregate rollback authority");
            }
            store_write_request_t rollback{
              {version_condition (root.key, root.value.version), lease,
               version_condition (item.key, item.value.version),
               version_condition (authority_key, stored->value.version)},
              {store_delete_t{item.key}}};
            if (stored->value.bytes != entry.before)
                rollback.mutations.push_back (
                  store_put_t{authority_key, entry.before, std::nullopt});
            if (!(co_await settle_batch (rollback))) {
                const auto current = co_await read (item.key);
                if (!std::holds_alternative<store_missing_t> (current))
                    invalid_format ("Could not roll back C++ v11 aggregate participant");
            }
            co_return;
        }
        co_await settle_child (root, lease, item, store_delete_t{item.key});
    }

    task_t<void> normalize_legacy_participant (const legacy_aggregate_t &aggregate,
                                               const store_scan_item_t &root,
                                               const store_condition_t &lease,
                                               const store_scan_item_t &item)
    {
        auto authority = parse_json (item.value.bytes);
        const bool dotnet = aggregate.writer == legacy_writer_t::dotnet;
        if (aggregate.state == legacy_state_t::committed) {
            const auto index = legacy_count (dotnet ? authority.at ("aggregateParticipantIndex")
                                                    : authority.at ("aggregate").at ("index"));
            const auto index_suffix = std::to_string (index);
            const bool java = aggregate.writer == legacy_writer_t::java;
            const auto child =
              dotnet
                ? root.key.value + ":participant:" + index_suffix
                : std::string (prefix) + (java ? "aggregate-inventory:" : "aggregate-participant:")
                    + aggregate.identity + (java ? ":participant:" : ":") + index_suffix;
            auto marker = dotnet ? json_t{} : authority.at ("aggregate");
            if (dotnet) {
                const store_key_t meta_key{child + ":meta"};
                const auto metadata = co_await read (meta_key);
                if (const auto *found = std::get_if<store_found_t> (&metadata))
                    marker = parse_json (found->value.bytes);
                else
                    marker = nullptr;
            }
            const store_key_t payload_key{child + (dotnet || java ? ":payload" : ":authority")};
            const auto payload = co_await read (payload_key);
            const auto *found = std::get_if<store_found_t> (&payload);
            if (!found || marker.is_null ()) {
                // Normalization precedes deletion of these children.
                const auto current = co_await read (item.key);
                if (const auto *after = std::get_if<store_found_t> (&current);
                    after && after->value.version.value != item.value.version.value)
                    co_return;
                const auto current_root = co_await read (root.key);
                if (std::holds_alternative<store_missing_t> (current_root))
                    co_return;
                invalid_format ("Missing committed v11 aggregate participant: " + item.key.value);
            }
            const auto digest = marker.at ("authorityPayloadSha256").get<std::string> ();
            const auto actual = sha256 (found->value.bytes);
            const bool valid_payload =
              dotnet
                ? base64_decode (digest) == std::vector<std::byte> (actual.begin (), actual.end ())
                : digest == hex (actual);
            if (!valid_payload
                || (dotnet
                    && (marker.at ("index") != index
                        || key_authority (marker.at ("key").get<std::string> ()).value
                             != item.key.value)))
                invalid_format ("Invalid committed v11 aggregate participant: " + item.key.value);
            authority["payload"] = base64_encode (found->value.bytes);
            if (java)
                authority["pendingCreation"] = nullptr;
            const auto &transition = marker.at ("ownerTransition");
            if (transition == "newOwner" || (dotnet && transition == 2)) {
                const auto generation =
                  legacy_generation (marker.at ("targetAuthorityOwnerGeneration"));
                if (generation <= legacy_generation (authority.at ("authorityOwnerGeneration")))
                    invalid_format ("Invalid v11 aggregate authority owner generation");
                authority["authorityOwnerGeneration"] = std::to_string (generation);
                authority["ownerId"] = aggregate.owner.owner_id;
                authority["ownerLeaseGeneration"] =
                  std::to_string (aggregate.owner.lease_generation);
                auto &allocation = authority.at ("allocation");
                if (java)
                    allocation["state"] = "active";
                allocation["descriptor"] = {{"meshName", aggregate.mesh},
                                            {"routingIdHex", aggregate.rid_hex}};
                allocation["descriptorLifecycleGeneration"] = std::to_string (aggregate.lifecycle);
            } else if (transition != "preserve" && (!dotnet || transition != 1)) {
                invalid_format ("Invalid v11 aggregate participant owner transition");
            }
        }
        authority.erase ("visibleStoreVersion");
        if (dotnet) {
            authority.erase ("aggregateFence");
            authority.erase ("aggregateParticipantIndex");
            authority.erase ("aggregateExpectedStoreVersion");
            if (aggregate.state == legacy_state_t::committed) {
                if (const auto relocation = aggregate.header.find ("relocation");
                    relocation != aggregate.header.end () && !relocation->is_null ()) {
                    authority["aggregate"] = {{"relocation", *relocation}};
                } else if (aggregate.header.value ("clearRelocation", false)) {
                    authority.erase ("aggregate");
                }
            }
        } else if (const auto relocation = authority.at ("aggregate").find ("relocation");
                   relocation != authority.at ("aggregate").end ()) {
            auto saved = *relocation;
            if (aggregate.writer == legacy_writer_t::java
                && aggregate.state == legacy_state_t::committed)
                saved["phase"] = "committed";
            authority["aggregate"] = {{"relocation", std::move (saved)}};
        } else {
            authority.erase ("aggregate");
        }
        co_await settle_child (root, lease, item,
                               store_put_t{item.key, to_bytes (authority.dump ()), std::nullopt});
    }

    task_t<void> settle_legacy_aggregate (store_scan_item_t root,
                                          const legacy_aggregate_t &aggregate,
                                          const store_condition_t &lease,
                                          const std::vector<store_scan_item_t> &participants)
    {
        // Keep the root discoverable until all children are settled. For Prepared,
        // the terminal transition and capacity return must commit together.
        const bool prepared = aggregate.state == legacy_state_t::prepared;
        const bool java_release = prepared && aggregate.writer == legacy_writer_t::java;
        if (prepared && !java_release) {
            auto record = parse_json (root.value.bytes);
            const auto state = legacy_state_field (record);
            record[state] = record.at (state).is_number () ? json_t (3) : json_t ("aborted");
            const auto aborted = to_bytes (record.dump ());
            store_write_request_t batch{{version_condition (root.key, root.value.version), lease},
                                        {store_put_t{root.key, aborted, std::nullopt}}};
            const auto before_release = co_await read (root.key);
            const auto *before = std::get_if<store_found_t> (&before_release);
            if (!before)
                co_return;
            if (before->value.version.value == root.value.version.value) {
                const auto release_available = co_await append_legacy_release (aggregate, batch);
                if (release_available)
                    (void) co_await settle_batch (batch);
            }
            const auto published = co_await read (root.key);
            const auto *found = std::get_if<store_found_t> (&published);
            if (!found)
                co_return;
            if (found->value.bytes != aborted
                && decode_legacy_aggregate ({root.key, found->value}).state
                     != legacy_state_t::aborted)
                invalid_format ("Could not abort v11 aggregate: " + root.key.value);
            root.value = found->value;
        }

        for (const auto &participant : participants) {
            if (aggregate.writer == legacy_writer_t::cpp)
                co_await settle_cpp_lock (aggregate, root, lease, participant);
            else
                co_await normalize_legacy_participant (aggregate, root, lease, participant);
        }
        if (aggregate.writer == legacy_writer_t::dotnet) {
            co_await delete_legacy_children (root, lease, root.key.value + ":participant:");
            co_await delete_legacy_children (root, lease, root.key.value + ":inventory:");
        } else {
            const auto suffix = aggregate.identity + ":";
            co_await delete_legacy_children (
              root, lease, std::string (prefix) + "aggregate-inventory:" + suffix);
            if (aggregate.writer == legacy_writer_t::cpp) {
                co_await delete_legacy_children (
                  root, lease, std::string (prefix) + "aggregate-inventory-index:" + suffix);
                co_await delete_legacy_children (
                  root, lease, std::string (prefix) + "aggregate-commit:" + suffix);
            } else if (aggregate.writer == legacy_writer_t::node) {
                co_await delete_legacy_children (
                  root, lease, std::string (prefix) + "aggregate-participant:" + suffix);
            }
        }
        store_write_request_t remove_root{{version_condition (root.key, root.value.version), lease},
                                          {store_delete_t{root.key}}};
        // Java v11 has no Aborted state. Deleting its root is the abort publication.
        bool ready = true;
        if (java_release)
            ready = co_await append_legacy_release (aggregate, remove_root);
        if (ready) {
            if (co_await settle_batch (remove_root))
                co_return;
        }
        const auto current = co_await read (root.key);
        if (!std::holds_alternative<store_missing_t> (current))
            invalid_format ("Could not remove v11 aggregate root: " + root.key.value);
    }

    task_t<std::optional<std::uint64_t>> settle_legacy_aggregates ()
    {
        auto rows = co_await scan_legacy_rows (std::string (aggregate_prefix));
        if (const auto complete = co_await read_completed_floor (); complete)
            co_return complete;
        std::map<legacy_fence_t, legacy_writer_t> writers;
        struct settlement_t
        {
            store_scan_item_t item;
            legacy_aggregate_t aggregate;
            store_condition_t lease;
        };
        std::vector<settlement_t> aggregates;
        for (auto &item : rows) {
            if (legacy_inline_child (item.key.value))
                continue;
            const auto current = co_await read (item.key);
            const auto *found = std::get_if<store_found_t> (&current);
            if (!found)
                continue;
            item.value = found->value;
            auto aggregate = decode_legacy_aggregate (item);
            // Preflight every root before mutating any of them.
            auto lease = co_await expired_target_condition (aggregate);
            if (!writers.emplace (legacy_fence (aggregate), aggregate.writer).second)
                invalid_format ("Duplicate v11 aggregate fence: " + item.key.value);
            aggregates.push_back ({std::move (item), std::move (aggregate), std::move (lease)});
        }
        if (aggregates.empty ())
            co_return std::nullopt;
        // Shared target capacity is updated by every startup in the same root order.
        std::sort (aggregates.begin (), aggregates.end (), [] (const auto &a, const auto &b) {
            return a.item.key.value < b.item.key.value;
        });
        legacy_participants_t participants;
        try {
            participants = co_await scan_legacy_participants (writers);
        }
        catch (const json_t::exception &) {
            invalid_format ("Invalid v11 aggregate participant record");
        }
        catch (const std::invalid_argument &) {
            invalid_format ("Invalid v11 aggregate participant identity");
        }
        if (const auto complete = co_await read_completed_floor (); complete)
            co_return complete;
        const std::vector<store_scan_item_t> none;
        for (auto &[item, aggregate, lease] : aggregates) {
            const auto selected = participants.find (legacy_fence (aggregate));
            try {
                co_await settle_legacy_aggregate (
                  std::move (item), aggregate, lease,
                  selected == participants.end () ? none : selected->second);
            }
            catch (const json_t::exception &) {
                invalid_format ("Invalid participant of v11 aggregate: " + aggregate.identity);
            }
            catch (const std::invalid_argument &) {
                invalid_format ("Invalid participant of v11 aggregate: " + aggregate.identity);
            }
        }
        co_return std::nullopt;
    }

    task_t<std::uint64_t> ensure_floor ()
    {
        const auto floor = co_await read (generation_floor_key);
        if (const auto *stored = std::get_if<store_found_t> (&floor))
            co_return migration_generation (stored->value.bytes);

        const store_key_t legacy_object{"zlink:v11:object-counter"};
        const store_key_t legacy_owner{"zlink:v11:authority-owner-counter"};
        const auto object = co_await read (legacy_object);
        const auto owner = co_await read (legacy_owner);
        const auto candidate =
          std::max (migration_generation (object), migration_generation (owner));
        const auto bytes =
          co_await put_missing (generation_floor_key, to_bytes (std::to_string (candidate)));
        co_return migration_generation (bytes);
    }

    static bool applied (const result_t<store_write_result_t> &result)
    {
        if (!result) {
            if (result.error_kind () == framework_error_kind_t::unavailable)
                return false;
            (void) result.value ();
        }
        if (std::holds_alternative<store_write_corrupt_t> (result.value ()))
            invalid_format ("The Location Store rejected a conversion batch");
        return std::holds_alternative<store_write_applied_t> (result.value ());
    }

    task_t<std::vector<std::byte>> put_missing (store_key_t key, std::vector<std::byte> bytes)
    {
        const store_write_request_t request{{store_missing_condition_t{key}},
                                            {store_put_t{key, bytes, std::nullopt}}};
        for (;;) {
            check_cancelled ();
            const auto result = co_await await_result (_store.write (request));
            if (applied (result))
                co_return bytes;
            const auto current = co_await read (key);
            if (const auto *found = std::get_if<store_found_t> (&current))
                co_return found->value.bytes;
            // An unavailable write with no durable result fails this startup.
            (void) result.value ();
        }
    }

    static std::string decode_component (std::string_view encoded)
    {
        std::string decoded;
        decoded.reserve (encoded.size ());
        for (std::size_t i = 0; i < encoded.size (); ++i) {
            if (encoded[i] != '%') {
                decoded += encoded[i];
                continue;
            }
            unsigned byte = 0;
            if (encoded.size () - i < 3)
                invalid_format ("Malformed percent escape in v11 Location Store capacity key");
            const auto end = encoded.data () + i + 3;
            const auto parsed = std::from_chars (end - 2, end, byte, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                invalid_format ("Malformed percent escape in v11 Location Store capacity key");
            decoded += static_cast<char> (byte);
            i += 2;
        }
        return decoded;
    }

    static std::pair<std::string, std::string> decode_capacity_key (const store_key_t &key)
    {
        const auto suffix = std::string_view (key.value).substr (legacy_capacity.size ());
        const auto separator = suffix.find (':');
        if (separator == 0 || separator == std::string_view::npos || separator + 1 == suffix.size ()
            || suffix.find (':', separator + 1) != std::string_view::npos)
            invalid_format ("Malformed v11 capacity key: " + key.value);
        auto mesh = decode_component (suffix.substr (0, separator));
        if (mesh.find ('\0') != std::string::npos)
            invalid_format ("Invalid v11 capacity mesh name");
        try {
            (void) json_t (mesh).dump ();
        }
        catch (const json_t::type_error &) {
            invalid_format ("Invalid UTF-8 in v11 capacity mesh name");
        }
        return {std::move (mesh), decode_component (suffix.substr (separator + 1))};
    }

    using descriptor_rids_t = std::map<std::string, std::set<std::string>>;

    task_t<descriptor_rids_t> read_mesh_rids (const std::string &mesh)
    {
        descriptor_rids_t rids;
        std::optional<store_scan_cursor_t> cursor;
        for (;;) {
            const auto scan_result = co_await scan (prefix_mesh (mesh), cursor);
            const auto *page = std::get_if<store_scan_page_t> (&scan_result);
            if (!page) {
                rids.clear ();
                cursor.reset ();
                continue;
            }
            for (const auto &item : page->items) {
                try {
                    const auto row = parse_json (item.value.bytes);
                    const auto rid =
                      zlink::routing_id_t::from_hex (row.at (location_record_fields::descriptor)
                                                       .at (location_record_fields::routingIdHex)
                                                       .get<std::string> ());
                    const auto rid_hex = rid.to_hex ();
                    rids[raw_rid (rid)].insert (rid_hex);
                    rids[rid.to_string ()].insert (rid_hex);
                    rids[foreign_rid_display (rid)].insert (rid_hex);
                }
                catch (const json_t::exception &) {
                    invalid_format ("Invalid descriptor while converting v11 capacity: "
                                    + item.key.value);
                }
                catch (const std::invalid_argument &) {
                    invalid_format ("Invalid descriptor RID while converting v11 capacity: "
                                    + item.key.value);
                }
            }
            cursor = page->next_cursor;
            if (!cursor)
                co_return rids;
        }
    }

    static std::string resolve_rid (const std::string &raw, const descriptor_rids_t &rids)
    {
        const auto matches = rids.find (raw);
        if (matches != rids.end ()) {
            if (matches->second.size () != 1)
                invalid_format ("Ambiguous RoutingId in v11 capacity key");
            return *matches->second.begin ();
        }
        try {
            // Binding RoutingId limits literal values to 255 bytes.
            if (raw.size () > 255 && raw.starts_with ("hex:"))
                return zlink::routing_id_t::from_hex (raw.substr (4)).to_hex ();
            return zlink::routing_id_t::from (raw).to_hex ();
        }
        catch (const std::invalid_argument &) {
            invalid_format ("Invalid RoutingId in v11 capacity key");
        }
    }

    static std::string dimension (std::string_view legacy, bool nested)
    {
        const auto separator = legacy.find (nested ? '\0' : ':');
        if (separator == std::string_view::npos)
            invalid_format ("Invalid v11 capacity spot type");
        const auto kind = legacy.substr (0, separator);
        const auto type = legacy.substr (separator + 1);
        if (type.empty () || type.find ('\0') != std::string_view::npos)
            invalid_format ("Invalid v11 capacity spot type");
        if (kind == (nested ? "user_spot" : "2"))
            return capacity_type_dimension (placement_object_kind_t::user_spot, type);
        if (kind == (nested ? "instance_spot" : "3"))
            return capacity_type_dimension (placement_object_kind_t::instance_spot, type);
        invalid_format ("Invalid v11 capacity spot kind");
    }

    static std::uint64_t read_capacity_count (const json_t &value)
    {
        const auto parsed =
          value.is_number_unsigned () ? parse_location_counter (value.dump ()) : std::nullopt;
        if (!parsed)
            invalid_format ("Invalid v11 capacity count");
        return static_cast<std::uint64_t> (*parsed);
    }

    template <typename Visit> static void for_each_capacity_count (json_t &record, Visit visit)
    {
        const bool nested = record.contains (location_record_fields::active)
                            || record.contains (location_record_fields::pending);
        for (const std::string_view state : {"active", "pending"}) {
            const bool active = state == "active";
            auto &usage = nested ? record.at (state) : record;
            visit ("actors", state,
                   usage.at (nested ? "actors" : (active ? "actorsActive" : "actorsPending")));
            visit ("spots", state,
                   usage.at (nested ? "spots" : (active ? "spotsActive" : "spotsPending")));
            auto &types = usage.at ("spotTypes");
            if (!types.is_object ())
                invalid_format ("Invalid v11 capacity spot types");
            for (auto &[type, value] : types.items ())
                visit (dimension (type, nested), state, nested ? value : value.at (state));
        }
    }

    static store_write_request_t capacity_move (const std::string &scope,
                                                const store_key_t &key,
                                                const std::vector<std::byte> &bytes)
    {
        store_write_request_t request{{store_value_condition_t{key, bytes}}, {}};
        try {
            auto record = parse_json (bytes);
            for_each_capacity_count (
              record, [&] (std::string_view name, std::string_view state, const json_t &value) {
                  const auto count = read_capacity_count (value);
                  if (count != 0) {
                      store_key_t counter{capacity_counter_key (scope, name, state)};
                      request.conditions.push_back (store_missing_condition_t{counter});
                      request.mutations.push_back (store_put_t{
                        std::move (counter), to_bytes (std::to_string (count)), std::nullopt});
                  }
              });
        }
        catch (const json_t::exception &) {
            invalid_format ("Invalid v11 capacity record: " + key.value);
        }
        request.mutations.push_back (store_delete_t{key});
        if (!batch_fits (request))
            invalid_format ("v11 capacity conversion exceeds Store batch limits: " + key.value);
        return request;
    }

    task_t<void> move_capacity_row (store_key_t key)
    {
        auto [mesh, raw] = decode_capacity_key (key);
        auto descriptors = _descriptor_rids.find (mesh);
        if (descriptors == _descriptor_rids.end ()) {
            auto rids = co_await read_mesh_rids (mesh);
            descriptors = _descriptor_rids.emplace (mesh, std::move (rids)).first;
        }
        // Recheck the old row after reading descriptors that another startup may
        // now publish.
        auto current = co_await read (key);
        if (!std::holds_alternative<store_found_t> (current))
            co_return;
        const auto scope = capacity_prefix (mesh, resolve_rid (raw, descriptors->second));
        for (;;) {
            const auto bytes = std::get<store_found_t> (current).value.bytes;
            const auto request = capacity_move (scope, key, bytes);
            check_cancelled ();
            const auto result = co_await await_result (_store.write (request));
            if (applied (result))
                co_return;

            std::vector<store_key_t> absent;
            for (const auto &mutation : request.mutations) {
                if (const auto *put = std::get_if<store_put_t> (&mutation)) {
                    auto destination = co_await read (put->key);
                    if (std::holds_alternative<store_missing_t> (destination))
                        absent.push_back (put->key);
                }
            }
            current = co_await read (key);
            if (std::holds_alternative<store_missing_t> (current)) {
                for (const auto &destination : absent) {
                    auto observed = co_await read (destination);
                    if (std::holds_alternative<store_missing_t> (observed))
                        invalid_format ("Converted v11 capacity counter is missing: " + key.value);
                }
                co_return;
            }
            if (absent.size () + 1 != request.mutations.size ()
                && bytes == std::get<store_found_t> (current).value.bytes)
                invalid_format ("A destination counter already exists for v11 capacity: "
                                + key.value);
            (void) result.value ();
        }
    }

    task_t<void> move_capacity ()
    {
        std::optional<store_scan_cursor_t> cursor;
        for (;;) {
            const auto scan_result = co_await scan (std::string (legacy_capacity), cursor);
            const auto *page = std::get_if<store_scan_page_t> (&scan_result);
            if (!page) {
                cursor.reset ();
                continue;
            }
            if (!cursor && page->items.empty () && !page->next_cursor)
                co_return;
            for (const auto &item : page->items)
                co_await move_capacity_row (item.key);
            cursor = page->next_cursor;
        }
    }

    location_store_t &_store;
    std::stop_token _cancellation;
    std::map<std::string, descriptor_rids_t> _descriptor_rids;
};

task_t<void> provider_location_repository_t::ensure_store_format (std::stop_token cancellation)
{
    if (_generation_floor.load (std::memory_order_relaxed) != 0)
        co_return;
    store_format_t format (_store, cancellation);
    const auto floor = co_await format.convert ();
    _generation_floor.store (floor, std::memory_order_relaxed);
}
} // namespace zlink::framework::runtime
