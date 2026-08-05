/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/dispatch_limits.hpp"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace zlink::framework::runtime::stateful
{

enum class object_kind_t
{
    actor,
    user_spot,
    instance_spot
};

enum class object_state_t
{
    creating,
    ready,
    moving,
    closing,
    recovering
};

enum class stateful_error_t
{
    none,
    invalid,
    not_found,
    type_mismatch,
    already_exists,
    generation_stale,
    moving,
    conflict,
    backpressured,
    instance_manager_create_forbidden
};

struct object_ref_t
{
    object_kind_t kind = object_kind_t::actor;
    std::string key;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::string mesh_name;
    std::string node_id;

    friend bool operator== (const object_ref_t &, const object_ref_t &) = default;
};

struct placement_candidate_t
{
    std::string mesh_name;
    std::string node_id;
    std::set<std::string> stable_types;
    int weight = 100;
    std::size_t active_capacity = 0;
    std::size_t active_count = 0;
    std::size_t pending_capacity = 0;
    std::size_t pending_count = 0;
};

struct create_request_t
{
    object_kind_t kind = object_kind_t::actor;
    std::string key;
    std::string stable_type;
    std::optional<std::string> mesh_name;
    std::vector<std::uint8_t> creation_request;
    bool exclusive = false;
    bool instance_intent = false;
};

enum class create_status_t
{
    reserved,
    joined,
    existing,
    failed
};

struct create_result_t
{
    create_status_t status = create_status_t::failed;
    stateful_error_t error = stateful_error_t::none;
    std::uint64_t attempt = 0;
    object_ref_t object;
    bool factory_owner = false;
};

struct membership_token_t
{
    std::uint64_t value = 0;
    object_ref_t actor;
    object_ref_t target_spot;

    friend bool operator== (const membership_token_t &,
                            const membership_token_t &) = default;
};

struct spot_close_token_t
{
    std::uint64_t value = 0;
    object_ref_t spot;

    friend bool operator== (const spot_close_token_t &,
                            const spot_close_token_t &) = default;
};

enum class turn_domain_t
{
    application,
    infrastructure
};

struct turn_record_t
{
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;

    friend bool operator== (const turn_record_t &, const turn_record_t &) = default;
};

struct logical_timer_t
{
    std::uint64_t timer_id = 0;
    std::uint64_t due_after_milliseconds = 0;
    std::uint64_t period_milliseconds = 0;
    std::uint64_t next_tick_sequence = 1;

    friend bool operator== (const logical_timer_t &,
                            const logical_timer_t &) = default;
};

struct frozen_object_state_t
{
    object_ref_t owner;
    std::string stable_type;
    std::vector<std::uint8_t> application_state;
    std::vector<turn_record_t> pending_application;
    std::vector<logical_timer_t> timers;

    friend bool operator== (const frozen_object_state_t &,
                            const frozen_object_state_t &) = default;
};

struct relocation_restore_identity_t
{
    std::string reference;
    std::uint32_t checksum_crc32c = 0;
    std::array<std::uint8_t, 32> inventory_digest{};

    friend bool operator== (const relocation_restore_identity_t &,
                            const relocation_restore_identity_t &) = default;
};

struct relocation_seal_t
{
    std::uint64_t token = 0;
    frozen_object_state_t frozen;
};

struct object_inventory_t
{
    object_ref_t owner;
    std::string stable_type;
    object_state_t state = object_state_t::creating;
    std::string membership;

    friend bool operator== (const object_inventory_t &,
                            const object_inventory_t &) = default;
};

struct aggregate_relocation_seal_t
{
    std::uint64_t token = 0;
    std::vector<frozen_object_state_t> participants;
};

class stateful_object_runtime_t
{
  public:
    using relocation_state_capture_t = std::function<
      std::vector<std::uint8_t> (
        const object_ref_t &, const std::string &, std::stop_token)>;
    using relocation_state_restore_t = std::function<
      bool (
        const frozen_object_state_t &, const object_ref_t &,
        std::stop_token)>;

    explicit stateful_object_runtime_t (
      std::size_t application_capacity =
        dispatch_limits::application_mailbox_messages,
      std::size_t infrastructure_capacity =
        dispatch_limits::control_mailbox_messages,
      std::size_t application_byte_capacity =
        dispatch_limits::application_mailbox_bytes,
      std::size_t infrastructure_byte_capacity =
        dispatch_limits::control_mailbox_bytes);

    void configure_relocation_state (
      relocation_state_capture_t capture,
      relocation_state_restore_t restore);

    void replace_placement_candidates (
      std::vector<placement_candidate_t> candidates);
    create_result_t begin_create (const create_request_t &request);
    create_result_t begin_reserved_object (
      const object_ref_t &reserved,
      const std::string &stable_type,
      std::vector<std::uint8_t> creation_request);
    // Accepts the next authority fence for an actor that is retained as a
    // remote copy while ownership returns to this node.
    stateful_error_t adopt_reserved_actor_owner (
      const object_ref_t &reserved,
      const std::string &stable_type);
    stateful_error_t commit_create (std::uint64_t attempt);
    stateful_error_t abort_create (std::uint64_t attempt);
    create_result_t activate_instance (
      create_request_t request,
      const std::function<bool (const object_ref_t &)> &factory);
    std::optional<object_ref_t> find (object_kind_t kind,
                                      const std::string &key) const;

    std::pair<stateful_error_t, membership_token_t> begin_membership_move (
      const object_ref_t &actor,
      const object_ref_t &target_spot);
    std::pair<stateful_error_t, membership_token_t>
    begin_remote_membership_move (
      const object_ref_t &actor,
      object_ref_t target_spot);
    std::pair<stateful_error_t, object_ref_t> commit_membership_move (
      const membership_token_t &token);
    stateful_error_t abort_membership_move (const membership_token_t &token);
    std::optional<std::string> actor_membership (
      const object_ref_t &actor) const;
    stateful_error_t destroy_actor (const object_ref_t &actor);
    std::pair<stateful_error_t, bool> close_spot (const object_ref_t &spot);
    std::pair<stateful_error_t, std::optional<spot_close_token_t>>
    begin_close_spot (const object_ref_t &spot);
    stateful_error_t commit_close_spot (const spot_close_token_t &token);
    stateful_error_t abort_close_spot (const spot_close_token_t &token);

    stateful_error_t enqueue (const object_ref_t &owner,
                              turn_domain_t domain,
                              turn_record_t record);
    std::pair<stateful_error_t, std::optional<turn_record_t>> try_claim (
      const object_ref_t &owner,
      turn_domain_t domain);
    stateful_error_t complete_claim (const object_ref_t &owner,
                                     turn_domain_t domain);
    stateful_error_t yield_claim (const object_ref_t &owner,
                                  turn_record_t continuation);
    std::size_t pending (const object_ref_t &owner,
                         turn_domain_t domain) const;
    std::size_t pending_bytes (const object_ref_t &owner,
                               turn_domain_t domain) const;
    stateful_error_t discard_application (
      const object_ref_t &owner,
      std::uint64_t sequence);
    stateful_error_t register_timer (const object_ref_t &owner,
                                     logical_timer_t timer);
    stateful_error_t cancel_timer (const object_ref_t &owner,
                                   std::uint64_t timer_id);
    stateful_error_t enqueue_timer_tick (
      const object_ref_t &owner,
      std::uint64_t timer_id,
      std::vector<std::uint8_t> payload);
    std::vector<logical_timer_t> timers (
      const object_ref_t &owner) const;
    std::vector<object_inventory_t> inventory () const;
    std::optional<std::vector<object_inventory_t>>
    try_begin_maintenance_inventory ();
    void end_maintenance_inventory () noexcept;
    std::pair<stateful_error_t, relocation_seal_t>
    try_seal_relocation (
      const object_ref_t &owner,
      std::stop_token cancellation = {});
    std::pair<stateful_error_t, aggregate_relocation_seal_t>
    try_seal_relocation_aggregate (
      const std::vector<object_ref_t> &participants,
      std::stop_token cancellation = {});
    stateful_error_t abort_relocation (std::uint64_t token);
    std::pair<stateful_error_t, object_ref_t>
    commit_relocation (std::uint64_t token, std::string target_node_id);
    std::pair<stateful_error_t, std::vector<object_ref_t>>
    commit_relocation_aggregate (
      std::uint64_t token, std::string target_node_id);
    stateful_error_t restore_relocation (
      frozen_object_state_t frozen,
      object_ref_t target,
      relocation_restore_identity_t identity,
      std::stop_token cancellation = {});
    stateful_error_t restore_relocation_aggregate (
      std::vector<frozen_object_state_t> frozen,
      std::vector<object_ref_t> targets,
      relocation_restore_identity_t identity,
      std::stop_token cancellation = {});
    stateful_error_t commit_relocation_restore (
      const object_ref_t &target,
      const relocation_restore_identity_t &identity);
    stateful_error_t commit_relocation_restore_aggregate (
      const std::vector<object_ref_t> &targets,
      const relocation_restore_identity_t &identity);
    stateful_error_t abort_relocation_restore (
      const object_ref_t &target,
      const relocation_restore_identity_t &identity);
    stateful_error_t abort_relocation_restore_aggregate (
      const std::vector<object_ref_t> &targets,
      const relocation_restore_identity_t &identity);

  private:
    struct object_key_t
    {
        object_kind_t kind;
        std::string key;

        bool operator< (const object_key_t &other) const noexcept;
        friend bool operator== (const object_key_t &,
                                const object_key_t &) = default;
    };

    struct queue_t
    {
        std::deque<turn_record_t> application;
        std::deque<turn_record_t> held_application;
        std::deque<turn_record_t> infrastructure;
        std::optional<turn_record_t> yielded_continuation;
        std::size_t application_bytes = 0;
        std::size_t held_application_bytes = 0;
        std::size_t infrastructure_bytes = 0;
        std::size_t application_active_bytes = 0;
        std::size_t infrastructure_active_bytes = 0;
        bool application_active = false;
        bool infrastructure_active = false;
    };

    struct object_record_t
    {
        object_ref_t reference;
        std::string stable_type;
        object_state_t state = object_state_t::creating;
        std::uint64_t attempt = 0;
        std::string membership;
        std::optional<relocation_restore_identity_t> restore_identity;
        queue_t queue;
        std::map<std::uint64_t, logical_timer_t> timers;
        std::uint64_t barrier_generation = 0;
    };

    struct membership_move_t
    {
        membership_token_t token;
        std::string previous_membership;
    };

    struct relocation_seal_state_t
    {
        std::vector<object_key_t> keys;
        std::vector<object_ref_t> sources;
        std::vector<frozen_object_state_t> frozen;
    };

    struct relocation_hold_state_t
    {
        std::size_t record_count = 0;
        std::size_t byte_count = 0;
    };

    static bool valid_text (const std::string &value);
    static bool same_exact_ref (const object_ref_t &left,
                                const object_ref_t &right);
    static object_key_t key_for (const object_ref_t &reference);
    object_record_t *find_record_locked (const object_ref_t &reference,
                                         stateful_error_t &error);
    const object_record_t *find_record_locked (
      const object_ref_t &reference,
      stateful_error_t &error) const;
    std::optional<placement_candidate_t> select_candidate_locked (
      const create_request_t &request) const;
    stateful_error_t enqueue_locked (object_record_t &object,
                                     turn_domain_t domain,
                                     turn_record_t record);
    static std::size_t retained_bytes (const turn_record_t &record) noexcept;
    void move_held_application_locked (object_record_t &object);
    void release_pending_capacity_locked (const object_record_t &record);

    const std::size_t _application_capacity;
    const std::size_t _infrastructure_capacity;
    const std::size_t _application_byte_capacity;
    const std::size_t _infrastructure_byte_capacity;
    mutable std::mutex _mutex;
    std::condition_variable _quiescence;
    std::vector<placement_candidate_t> _candidates;
    std::map<object_key_t, object_record_t> _objects;
    std::map<object_key_t, std::uint64_t> _last_generation;
    std::map<std::uint64_t, object_key_t> _attempts;
    std::map<std::uint64_t, membership_move_t> _membership_moves;
    std::map<std::uint64_t, spot_close_token_t> _spot_closes;
    std::map<std::uint64_t, relocation_seal_state_t> _relocation_seals;
    std::map<std::uint64_t, relocation_hold_state_t> _relocation_holds;
    std::map<object_key_t, std::uint64_t>
      _relocation_restore_reservations;
    relocation_state_capture_t _relocation_state_capture;
    relocation_state_restore_t _relocation_state_restore;
    bool _maintenance_inventory_active = false;
    std::uint64_t _next_attempt = 1;
    std::uint64_t _next_membership_token = 1;
    std::uint64_t _next_spot_close_token = 1;
    std::uint64_t _next_relocation_token = 1;
    std::uint64_t _next_relocation_restore_reservation = 1;
};

} // namespace zlink::framework::runtime::stateful
