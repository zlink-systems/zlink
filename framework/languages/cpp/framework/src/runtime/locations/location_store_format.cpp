/* SPDX-License-Identifier: Elastic-2.0 */

#include "provider_location_repository.hpp"

#include <charconv>

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
            completed = co_await check_aggregates ();
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

    static bool terminal_aggregate (const store_scan_item_t &item)
    {
        const auto &bytes = item.value.bytes;
        if (bytes.size () >= 25 && bytes.front () == std::byte{2}) {
            // Java's committed header is state, UUID, then big-endian generation.
            std::string id =
              zlink::framework::detail::encode_hex (std::span (bytes).subspan (1, 16));
            for (const auto offset : {20u, 16u, 12u, 8u})
                id.insert (offset, "-");
            std::uint64_t generation = 0;
            for (const auto value : std::span (bytes).subspan (17, 8))
                generation = (generation << 8) | std::to_integer<unsigned> (value);
            return generation <= max_generation
                   && item.key.value
                        == std::string (aggregate_prefix) + id + ":" + std::to_string (generation);
        }
        const auto record = json_t::parse (to_string (bytes), nullptr, false);
        if (!record.is_object ())
            return false;
        auto state = record.find ("state");
        if (state == record.end () || state->is_null ())
            state = record.find (location_record_fields::status);
        return state != record.end ()
               && (*state == aggregate_status::committed || *state == aggregate_status::aborted
                   || *state == 2 || *state == 3);
    }

    task_t<std::optional<std::uint64_t>> check_aggregates ()
    {
        std::optional<store_scan_cursor_t> cursor;
        for (;;) {
            const auto scan_result = co_await scan (std::string (aggregate_prefix), cursor);
            const auto *page = std::get_if<store_scan_page_t> (&scan_result);
            if (!page) {
                cursor.reset ();
                continue;
            }
            for (const auto &item : page->items) {
                // .NET stores participant and inventory children under the parent prefix.
                const auto suffix =
                  std::string_view (item.key.value).substr (aggregate_prefix.size ());
                const auto generation = suffix.find (':');
                if (generation != std::string_view::npos) {
                    const auto child = suffix.find (':', generation + 1);
                    if (child != std::string_view::npos
                        && (suffix.substr (child).starts_with (":participant:")
                            || suffix.substr (child).starts_with (":inventory:")))
                        continue;
                }
                if (!terminal_aggregate (item)) {
                    // A different startup may have published the marker before this
                    // snapshot.
                    const auto completed = co_await read_completed_floor ();
                    if (completed)
                        co_return completed;
                    invalid_format ("Unfinished v11 aggregate requires v11 termination: "
                                    + item.key.value);
                }
            }
            cursor = page->next_cursor;
            if (!cursor)
                co_return std::nullopt;
        }
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

    static store_write_request_t capacity_move (const std::string &scope,
                                                const store_key_t &key,
                                                const std::vector<std::byte> &bytes)
    {
        store_write_request_t request{{store_value_condition_t{key, bytes}}, {}};
        try {
            const auto record = parse_json (bytes);
            const bool nested = record.contains (location_record_fields::active)
                                || record.contains (location_record_fields::pending);
            const auto add = [&] (std::string_view dimension_name, std::string_view state,
                                  const json_t &value) {
                if (!value.is_number_unsigned ())
                    invalid_format ("Invalid v11 capacity count: " + key.value);
                const auto decimal = value.dump ();
                const auto parsed = parse_location_counter (decimal);
                if (!parsed)
                    invalid_format ("Invalid v11 capacity count: " + key.value);
                if (*parsed != 0) {
                    store_key_t counter{capacity_counter_key (scope, dimension_name, state)};
                    request.conditions.push_back (store_missing_condition_t{counter});
                    request.mutations.push_back (
                      store_put_t{std::move (counter), to_bytes (decimal), std::nullopt});
                }
            };
            for (const std::string_view state : {"active", "pending"}) {
                const bool active = state == "active";
                const auto &usage = nested ? record.at (state) : record;
                add ("actors", state,
                     usage.at (nested ? "actors" : (active ? "actorsActive" : "actorsPending")));
                add ("spots", state,
                     usage.at (nested ? "spots" : (active ? "spotsActive" : "spotsPending")));
                const auto &types = usage.at ("spotTypes");
                if (!types.is_object ())
                    invalid_format ("Invalid v11 capacity spot types: " + key.value);
                for (const auto &[type, value] : types.items ())
                    add (dimension (type, nested), state, nested ? value : value.at (state));
            }
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
