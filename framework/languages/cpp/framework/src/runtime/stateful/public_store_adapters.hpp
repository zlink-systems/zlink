/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/stateful/maintenance_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/sha256.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <memory>
#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace zlink::framework::runtime::stateful
{

class public_aggregate_authority_adapter_t;

class public_relocation_store_adapter_t final :
    public relocation_store_port_t
{
  public:
    explicit public_relocation_store_adapter_t (
      std::shared_ptr<zlink::framework::relocation_repository_t> store) :
        _owner (std::move (store)), _store (_owner.get ())
    {
        if (!_store)
            throw std::invalid_argument (
              "relocation store must not be null");
    }

    explicit public_relocation_store_adapter_t (
      zlink::framework::relocation_repository_t &store) noexcept :
        _store (&store)
    {
    }

    relocation_stored_t put (
      const std::vector<std::uint8_t> &payload,
      std::chrono::hours retention) override
    {
        std::vector<std::byte> public_payload;
        public_payload.reserve (payload.size ());
        for (const auto value : payload)
            public_payload.push_back (static_cast<std::byte> (value));
        const auto stored =
          _store
            ->put_relocation (
              std::move (public_payload), retention)
            .result ()
            .value ();
        return {stored.reference, stored.checksum_crc32c};
    }

    std::optional<std::vector<std::uint8_t>>
    get (const std::string &reference) override
    {
        const auto read =
          _store->get_relocation (reference).result ().value ();
        const auto *found =
          std::get_if<zlink::framework::relocation_found_t> (&read);
        if (!found)
            return std::nullopt;
        std::vector<std::uint8_t> payload;
        payload.reserve (found->payload.size ());
        for (const auto value : found->payload)
            payload.push_back (std::to_integer<std::uint8_t> (value));
        return payload;
    }

    void remove (const std::string &reference) override
    {
        (void) _store->delete_relocation (reference).result ().value ();
    }

  private:
    std::shared_ptr<zlink::framework::relocation_repository_t> _owner;
    zlink::framework::relocation_repository_t *_store;
};

class public_authority_store_adapter_t final :
    public authority_relocation_port_t
{
  public:
    explicit public_authority_store_adapter_t (
      zlink::framework::location_repository_t &store) noexcept :
        _store (&store)
    {
    }

    authority_publish_result_t publish (
      const object_ref_t &source,
      std::string target_node_id,
      location_owner_token_t target_owner,
      relocation_capacity_fence_t relocation_capacity_fence,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest) override
    {
        const auto key = authority_key (source);
        const auto read =
          _store->read_authority (key).result ().value ();
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&read);
        if (!snapshot
            || snapshot->object_generation
                 != source.object_generation
            || snapshot->authority_owner_generation
                 != source.authority_owner_generation)
            return {authority_publish_status_t::conflict,
                    decode_current (read)};

        authority_relocation_reference_t reference{
          source,
          source,
          std::move (relocation_reference),
          checksum_crc32c,
          inventory_digest,
          target_owner,
          snapshot->payload};
        reference.target.node_id = std::move (target_node_id);
        reference.target.authority_owner_generation =
          source.authority_owner_generation + 1;
        const auto exchanged =
          _store
            ->compare_exchange_authority (
              key,
              snapshot->store_version,
              authority_put_t{
                encode (reference),
                authority_generation_transition_t::new_owner,
                target_owner,
                std::move (relocation_capacity_fence)})
            .result ()
            .value ();
        if (const auto *stored =
              std::get_if<authority_stored_t> (&exchanged)) {
            auto current = decode (stored->snapshot);
            if (!current
                || !same_owner (
                  stored->snapshot.owner, target_owner))
                return {authority_publish_status_t::failed,
                        std::move (current)};
            return {authority_publish_status_t::published,
                    std::move (current)};
        }
        if (const auto *conflict =
              std::get_if<authority_conflict_t> (&exchanged))
            return {authority_publish_status_t::conflict,
                    decode_current (conflict->current)};
        return {authority_publish_status_t::failed, std::nullopt};
    }

    std::optional<authority_relocation_reference_t>
    read (object_kind_t kind, const std::string &key) override
    {
        const auto result =
          _store
            ->read_authority (
              authority_key (
                object_ref_t{.kind = kind, .key = key}))
            .result ()
            .value ();
        return decode_current (result);
    }

    authority_publish_result_t publish_completion (
      object_kind_t kind,
      const std::string &key,
      const std::string &mesh_name,
      std::uint64_t object_generation,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c) override
    {
        const auto authority =
          authority_key (object_ref_t{.kind = kind, .key = key});
        const auto read =
          _store->read_authority (authority).result ().value ();
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&read);
        if (!snapshot
            || snapshot->object_generation != object_generation)
            return {authority_publish_status_t::conflict,
                    decode_current (read)};
        if (auto current = decode (*snapshot)) {
            return current->relocation_reference
                         == relocation_reference
                       && current->checksum_crc32c
                            == checksum_crc32c
                     ? authority_publish_result_t{
                         authority_publish_status_t::published,
                         std::move (current)}
                     : authority_publish_result_t{
                         authority_publish_status_t::conflict,
                         std::move (current)};
        }

        object_ref_t actor{
          kind,
          key,
          snapshot->object_generation,
          snapshot->authority_owner_generation,
          mesh_name,
          snapshot->owner.owner_id};
        authority_relocation_reference_t reference{
          actor,
          actor,
          std::move (relocation_reference),
          checksum_crc32c,
          {},
          snapshot->owner,
          snapshot->payload};
        return store_preserving_authority (
          authority, *snapshot, std::move (reference));
    }

    authority_publish_result_t replace_completion (
      object_kind_t kind,
      const std::string &key,
      std::uint64_t object_generation,
      const std::string &expected_reference,
      std::uint32_t expected_checksum_crc32c,
      std::string next_reference,
      std::uint32_t next_checksum_crc32c) override
    {
        const auto authority =
          authority_key (object_ref_t{.kind = kind, .key = key});
        const auto read =
          _store->read_authority (authority).result ().value ();
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&read);
        auto current =
          snapshot ? decode (*snapshot) : std::nullopt;
        if (!snapshot || !current
            || snapshot->object_generation != object_generation
            || current->relocation_reference
                 != expected_reference
            || current->checksum_crc32c
                 != expected_checksum_crc32c)
            return {authority_publish_status_t::conflict,
                    std::move (current)};
        current->relocation_reference =
          std::move (next_reference);
        current->checksum_crc32c =
          next_checksum_crc32c;
        return store_preserving_authority (
          authority, *snapshot, std::move (*current));
    }

    bool release_completion (
      object_kind_t kind,
      const std::string &key,
      std::uint64_t object_generation,
      const std::string &expected_reference,
      std::uint32_t expected_checksum_crc32c) override
    {
        const auto authority =
          authority_key (object_ref_t{.kind = kind, .key = key});
        const auto read =
          _store->read_authority (authority).result ().value ();
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&read);
        const auto current =
          snapshot ? decode (*snapshot) : std::nullopt;
        if (!snapshot || !current
            || snapshot->object_generation != object_generation
            || current->relocation_reference
                 != expected_reference
            || current->checksum_crc32c
                 != expected_checksum_crc32c)
            return false;
        const auto exchanged =
          _store
            ->compare_exchange_authority (
              authority,
              snapshot->store_version,
              authority_put_t{
                current->application_payload,
                authority_generation_transition_t::preserve,
                std::nullopt,
                std::nullopt})
            .result ()
            .value ();
        return std::holds_alternative<authority_stored_t> (
          exchanged);
    }

  private:
    friend class public_aggregate_authority_adapter_t;
    authority_publish_result_t store_preserving_authority (
      const authority_key_t &key,
      const authority_snapshot_t &snapshot,
      authority_relocation_reference_t reference)
    {
        const auto expected_reference =
          reference.relocation_reference;
        const auto expected_checksum =
          reference.checksum_crc32c;
        const auto exchanged =
          _store
            ->compare_exchange_authority (
              key,
              snapshot.store_version,
              authority_put_t{
                encode (reference),
                authority_generation_transition_t::preserve,
                std::nullopt,
                std::nullopt})
            .result ()
            .value ();
        if (const auto *stored =
              std::get_if<authority_stored_t> (&exchanged)) {
            auto current = decode (stored->snapshot);
            return current
                       && current->relocation_reference
                            == expected_reference
                       && current->checksum_crc32c
                            == expected_checksum
                     ? authority_publish_result_t{
                         authority_publish_status_t::published,
                         std::move (current)}
                     : authority_publish_result_t{
                         authority_publish_status_t::failed,
                         std::move (current)};
        }
        if (const auto *conflict =
              std::get_if<authority_conflict_t> (&exchanged))
            return {authority_publish_status_t::conflict,
                    decode_current (conflict->current)};
        return {authority_publish_status_t::failed,
                std::nullopt};
    }

    static bool same_owner (
      const location_owner_token_t &left,
      const location_owner_token_t &right) noexcept
    {
        return left.owner_id == right.owner_id
               && left.lease_generation == right.lease_generation;
    }

    static authority_key_t authority_key (
      const object_ref_t &object)
    {
        const auto prefix =
          object.kind == object_kind_t::actor ? "1:" : "2:";
        return {std::string (prefix) + object.key};
    }

    static void append_u32 (
      std::vector<std::byte> &output,
      std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            output.push_back (
              static_cast<std::byte> (
                (value >> shift) & 0xffu));
    }

    static void append_u64 (
      std::vector<std::byte> &output,
      std::uint64_t value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
            output.push_back (
              static_cast<std::byte> (
                (value >> shift) & 0xffu));
    }

    static void append_text (
      std::vector<std::byte> &output,
      std::string_view value)
    {
        append_u32 (
          output, static_cast<std::uint32_t> (value.size ()));
        for (const auto character : value)
            output.push_back (
              static_cast<std::byte> (
                static_cast<unsigned char> (character)));
    }

    static std::vector<std::byte> encode (
      const authority_relocation_reference_t &reference)
    {
        std::vector<std::byte> output;
        for (const auto value : std::string_view ("ZLRA2"))
            output.push_back (
              static_cast<std::byte> (
                static_cast<unsigned char> (value)));
        output.push_back (
          static_cast<std::byte> (reference.source.kind));
        append_text (output, reference.source.key);
        append_text (output, reference.source.mesh_name);
        append_text (output, reference.source.node_id);
        append_u64 (output, reference.source.object_generation);
        append_u64 (
          output,
          reference.source.authority_owner_generation);
        append_text (output, reference.target.node_id);
        append_text (output, reference.relocation_reference);
        append_u32 (output, reference.checksum_crc32c);
        for (const auto value : reference.inventory_digest)
            output.push_back (static_cast<std::byte> (value));
        append_u32 (
          output,
          static_cast<std::uint32_t> (
            reference.application_payload.size ()));
        output.insert (
          output.end (),
          reference.application_payload.begin (),
          reference.application_payload.end ());
        return output;
    }

    class reader_t
    {
      public:
        explicit reader_t (
          const std::vector<std::byte> &input) :
            _input (input)
        {
        }

        bool consume_magic ()
        {
            constexpr std::string_view magic = "ZLRA2";
            if (_input.size () < magic.size ())
                return false;
            for (const auto value : magic) {
                if (read_byte ()
                    != static_cast<std::uint8_t> (value))
                    return false;
            }
            return true;
        }

        std::optional<std::uint8_t> byte ()
        {
            if (_offset >= _input.size ())
                return std::nullopt;
            return read_byte ();
        }

        std::optional<std::uint32_t> u32 ()
        {
            std::uint32_t value = 0;
            for (int index = 0; index != 4; ++index) {
                const auto next = byte ();
                if (!next)
                    return std::nullopt;
                value = (value << 8) | *next;
            }
            return value;
        }

        std::optional<std::uint64_t> u64 ()
        {
            std::uint64_t value = 0;
            for (int index = 0; index != 8; ++index) {
                const auto next = byte ();
                if (!next)
                    return std::nullopt;
                value = (value << 8) | *next;
            }
            return value;
        }

        std::optional<std::string> text ()
        {
            const auto size = u32 ();
            if (!size || _offset + *size > _input.size ())
                return std::nullopt;
            std::string value;
            value.reserve (*size);
            for (std::uint32_t index = 0; index != *size; ++index)
                value.push_back (
                  static_cast<char> (read_byte ()));
            return value;
        }

        bool done () const noexcept
        {
            return _offset == _input.size ();
        }

      private:
        std::uint8_t read_byte ()
        {
            return std::to_integer<std::uint8_t> (
              _input[_offset++]);
        }

        const std::vector<std::byte> &_input;
        std::size_t _offset = 0;
    };

    static std::optional<authority_relocation_reference_t>
    decode (const authority_snapshot_t &snapshot)
    {
        reader_t reader (snapshot.payload);
        if (!reader.consume_magic ())
            return std::nullopt;
        const auto kind = reader.byte ();
        const auto key = reader.text ();
        const auto mesh = reader.text ();
        const auto source_node = reader.text ();
        const auto object_generation = reader.u64 ();
        const auto source_owner_generation = reader.u64 ();
        const auto target_node = reader.text ();
        const auto relocation_reference = reader.text ();
        const auto checksum = reader.u32 ();
        if (!kind || !key || !mesh || !source_node
            || !object_generation || !source_owner_generation
            || !target_node || !relocation_reference || !checksum)
            return std::nullopt;
        inventory_digest_t digest{};
        for (auto &value : digest) {
            const auto next = reader.byte ();
            if (!next)
                return std::nullopt;
            value = *next;
        }
        const auto application_payload_size = reader.u32 ();
        if (!application_payload_size)
            return std::nullopt;
        std::vector<std::byte> application_payload;
        application_payload.reserve (
          *application_payload_size);
        for (std::uint32_t index = 0;
             index != *application_payload_size;
             ++index) {
            const auto next = reader.byte ();
            if (!next)
                return std::nullopt;
            application_payload.push_back (
              static_cast<std::byte> (*next));
        }
        if (!reader.done ())
            return std::nullopt;
        const auto object_kind =
          static_cast<object_kind_t> (*kind);
        if (object_kind != object_kind_t::actor
            && object_kind != object_kind_t::user_spot
            && object_kind != object_kind_t::instance_spot)
            return std::nullopt;
        object_ref_t source{
          object_kind,
          std::move (*key),
          *object_generation,
          *source_owner_generation,
          std::move (*mesh),
          std::move (*source_node)};
        auto target = source;
        target.node_id = std::move (*target_node);
        target.authority_owner_generation =
          snapshot.authority_owner_generation;
        return authority_relocation_reference_t{
          std::move (source),
          std::move (target),
          std::move (*relocation_reference),
          *checksum,
          digest,
          snapshot.owner,
          std::move (application_payload)};
    }

    static std::optional<authority_relocation_reference_t>
    decode_current (const authority_read_result_t &current)
    {
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&current);
        return snapshot ? decode (*snapshot) : std::nullopt;
    }

    zlink::framework::location_repository_t *_store;
};

class public_aggregate_authority_adapter_t final :
    public aggregate_authority_port_t
{
  public:
    explicit public_aggregate_authority_adapter_t (
      zlink::framework::location_repository_t &store) noexcept :
        _store (&store)
    {
    }

    aggregate_publish_result_t prepare (
      const std::vector<object_ref_t> &sources,
      std::string target_node_id,
      location_owner_token_t target_owner,
      std::vector<relocation_capacity_fence_t> relocation_capacity_fences,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest) override
    {
        if (sources.size () < 2 || relocation_capacity_fences.size () != sources.size ()
            || target_node_id.empty ()
            || target_owner.owner_id.empty ()
            || target_owner.lease_generation <= 0)
            return {};
        std::map<std::string, relocation_capacity_fence_t> capacity_by_authority;
        for (std::size_t index = 0; index < sources.size (); ++index) {
            if (relocation_capacity_fences[index].value.empty ())
                return {};
            const auto [_, inserted] = capacity_by_authority.emplace (
              public_authority_store_adapter_t::authority_key (sources[index]).value,
              std::move (relocation_capacity_fences[index]));
            if (!inserted)
                return {};
        }
        std::vector<std::pair<authority_snapshot_t,
                              authority_relocation_reference_t>>
          snapshots;
        snapshots.reserve (sources.size ());
        placement_capacity_bundle_t capacity;
        std::optional<spot_type_capacity_delta_t> spot_type;
        for (const auto &source : sources) {
            const auto read = _store->read_authority (
              public_authority_store_adapter_t::authority_key (
                source)).result ().value ();
            const auto *snapshot =
              std::get_if<authority_snapshot_t> (&read);
            if (!snapshot
                || snapshot->object_generation
                     != source.object_generation
                || snapshot->authority_owner_generation
                     != source.authority_owner_generation)
                return {aggregate_publish_status_t::conflict, {}, {}};
            auto target = source;
            target.node_id = target_node_id;
            ++target.authority_owner_generation;
            authority_relocation_reference_t reference{
              source, target, relocation_reference,
              checksum_crc32c, inventory_digest,
              target_owner, snapshot->payload};
            snapshots.emplace_back (*snapshot, std::move (reference));
            capacity.actor_slots +=
              snapshot->allocation.capacity_bundle.actor_slots;
            capacity.spot_slots +=
              snapshot->allocation.capacity_bundle.spot_slots;
            if (snapshot->allocation.capacity_bundle.spot_type)
                spot_type =
                  snapshot->allocation.capacity_bundle.spot_type;
        }
        capacity.spot_type = spot_type;
        const auto nodes =
          _store->list_mesh_nodes (
            sources.front ().mesh_name).result ().value ();
        const auto target_node = std::find_if (
          nodes.items.begin (), nodes.items.end (),
          [&] (const mesh_node_descriptor_t &node) {
              return node.rid.to_string () == target_node_id
                     && node.owner_id == target_owner.owner_id
                     && node.lease_generation
                          == target_owner.lease_generation;
          });
        if (target_node == nodes.items.end ())
            return {};

        std::vector<std::byte> seed;
        for (const auto value : relocation_reference)
            seed.push_back (static_cast<std::byte> (
              static_cast<unsigned char> (value)));
        for (const auto &source : sources) {
            for (const auto value : source.key)
                seed.push_back (static_cast<std::byte> (
                  static_cast<unsigned char> (value)));
        }
        const auto digest = runtime::sha256 (seed);
        aggregate_id_t aggregate_id;
        std::copy_n (
          digest.begin (), aggregate_id.value.size (),
          aggregate_id.value.begin ());
        const auto generation =
          _next_generation.fetch_add (1, std::memory_order_relaxed);

        std::vector<aggregate_participant_t> participants;
        participants.reserve (snapshots.size ());
        for (const auto &[snapshot, reference] : snapshots) {
            const auto capacity = capacity_by_authority.find (
              public_authority_store_adapter_t::authority_key (reference.source).value);
            if (capacity == capacity_by_authority.end ())
                return {};
            participants.push_back ({
              public_authority_store_adapter_t::authority_key (
                reference.source),
              snapshot.store_version,
              authority_generation_transition_t::new_owner,
              public_authority_store_adapter_t::encode (reference),
              {},
              std::optional<relocation_capacity_fence_t>{capacity->second}});
        }
        std::sort (
          participants.begin (), participants.end (),
          [] (const auto &left, const auto &right) {
              return left.key.value < right.key.value;
          });
        zlink::framework::inventory_digest_t public_digest;
        for (std::size_t index = 0;
             index != public_digest.value.size (); ++index)
            public_digest.value[index] =
              static_cast<std::byte> (inventory_digest[index]);
        aggregate_prepare_request_t request;
        request.aggregate_id = aggregate_id;
        request.aggregate_generation = generation;
        request.participants = std::move (participants);
        request.inventory_digest = public_digest;
        request.target_descriptor = {sources.front ().mesh_name, target_node->rid};
        request.target_descriptor_lifecycle_generation = target_node->lifecycle_generation;
        request.capacity_bundle = capacity;
        request.target_owner = target_owner;
        for (const auto &participant : request.participants) {
            if (!participant.capacity_fence)
                return {};
            request.capacity_fences.push_back (*participant.capacity_fence);
        }
        const auto prepared = _store->prepare_aggregate (std::move (request)).result ().value ();
        const auto *created =
          std::get_if<aggregate_prepared_t> (&prepared);
        const auto *existing =
          std::get_if<aggregate_already_prepared_t> (&prepared);
        if (!created && !existing)
            return {aggregate_publish_status_t::conflict, {}, {}};
        const auto public_fence =
          created ? created->fence : existing->fence;
        const auto local_fence = aggregate_relocation_fence_t{
          public_fence.aggregate_generation, public_fence};
        std::vector<authority_relocation_reference_t> current;
        current.reserve (snapshots.size ());
        for (auto &[_, reference] : snapshots)
            current.push_back (std::move (reference));
        return {aggregate_publish_status_t::prepared,
                local_fence, std::move (current)};
    }

    aggregate_publish_result_t commit (
      aggregate_relocation_fence_t fence) override
    {
        if (fence.value == 0
            || fence.durable_fence.aggregate_generation == 0)
            return {};
        const auto participants =
          _store->read_aggregate_participants (fence.durable_fence)
            .result ()
            .value ();
        if (!participants)
            return {};
        const auto committed =
          _store->commit_aggregate (
            fence.durable_fence).result ().value ();
        if (committed != aggregate_commit_result_t::committed
            && committed
                 != aggregate_commit_result_t::already_committed)
            return {aggregate_publish_status_t::conflict, fence, {}};
        std::vector<authority_relocation_reference_t> current;
        current.reserve (participants->size ());
        for (const auto &participant : *participants) {
            const auto read =
              _store->read_authority (
                participant.key).result ().value ();
            const auto decoded =
              public_authority_store_adapter_t::decode_current (
                read);
            if (!decoded)
                return {};
            current.push_back (*decoded);
        }
        return {aggregate_publish_status_t::committed,
                fence, std::move (current)};
    }

    void abort (aggregate_relocation_fence_t fence) override
    {
        if (fence.value != 0
            && fence.durable_fence.aggregate_generation != 0)
            (void) _store->abort_aggregate (
              fence.durable_fence).result ().value ();
    }

  private:
    zlink::framework::location_repository_t *_store;
    std::atomic<std::uint64_t> _next_generation{1};
};

} // namespace zlink::framework::runtime::stateful
