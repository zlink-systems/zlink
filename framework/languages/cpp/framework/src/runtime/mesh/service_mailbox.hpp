/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <string>
#include <variant>
#include <vector>

#include <zlink/Contracts/Messaging/received.hpp>
#include "runtime/dispatch/application_record.hpp"
#include "runtime/messaging/envelope_codec.hpp"

namespace zlink::framework
{
struct location_owner_token_t;
struct mesh_node_descriptor_t;
} // namespace zlink::framework

namespace zlink::framework::runtime::mesh
{

struct service_node_descriptor_t;

struct local_request_source_t
{
    std::shared_ptr<const location_owner_token_t> lease;
    std::shared_ptr<const service_node_descriptor_t> node;
};

// Location runtime §9.3: the source original accepted with a record. A peer's is the
// discovery original bound to its admission; a local one is this node's lease observation.
using request_source_t =
  std::variant<local_request_source_t, std::shared_ptr<const mesh_node_descriptor_t>>;

enum class service_mailbox_domain_t
{
    application,
    infrastructure
};

struct service_bound_session_source_t
{
    std::vector<std::uint8_t> session_routing_id;
    std::uint64_t binding_generation = 0;
    std::uint64_t session_sequence = 0;
};

struct service_mailbox_claim_t;
class service_mailbox_t;

// Claim credit of one claimed original. The claim mints it into the slot, so only the
// take that moves the original out holds it; a completion that lost the take holds none
// and cannot settle.
class service_mailbox_settlement_t
{
  public:
    service_mailbox_settlement_t () = default;

    explicit operator bool () const noexcept { return static_cast<bool> (_claim); }
    bool operator() () const;

  private:
    friend class service_mailbox_t;
    service_mailbox_settlement_t (std::shared_ptr<service_mailbox_claim_t> claim,
                                  std::size_t index) noexcept :
        _claim (std::move (claim)), _index (index)
    {
    }

    std::shared_ptr<service_mailbox_claim_t> _claim;
    std::size_t _index = 0;
};

struct service_mailbox_record_t
{
    std::string owner;
    service_mailbox_domain_t domain;
    std::vector<zlink::message_t> parts;
    std::vector<std::uint8_t> source_routing_id{};
    std::optional<zlink::reply_token_t> reply_token = std::nullopt;
    std::optional<std::uint64_t> correlation = std::nullopt;
    std::uint64_t source_node_generation = 0;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> operation;
    std::optional<service_bound_session_source_t> bound_session_source;
    std::function<void ()> before_application_handler;
    std::shared_ptr<host::local_application_dispatch_t> application;
    messaging::envelope_header_t envelope_header;
    service_mailbox_settlement_t transferred;
    // Read once by the publication owner before the mailbox accepts the record.
    std::optional<request_source_t> source_original = std::nullopt;
};

// A claimed slot: the claim and the slot's position in the claimed prefix.
struct service_mailbox_slot_t
{
    std::shared_ptr<service_mailbox_claim_t> claim;
    std::size_t index = 0;
};

class service_mailbox_t
{
  public:
    service_mailbox_t () = default;
    service_mailbox_t (const service_mailbox_t &) = delete;
    service_mailbox_t &operator= (const service_mailbox_t &) = delete;

    static std::string application_owner (host::owner_kind_t kind, std::string_view id = {});
    static std::string application_owner (const host::ready_record_t &owner);

    using application_prepare_t = std::function<void (service_mailbox_record_t &)>;
    using application_ready_t = std::function<void (const std::string &)>;
    using application_dispatch_t = std::function<void (
      const host::ready_record_t &, const host::receive_record_t &, std::vector<zlink::message_t>)>;
    using stateful_take_t = std::function<bool (const application_dispatch_t &)>;
    using stateful_ready_t = std::function<task_t<bool> (const std::string &, stateful_take_t)>;
    void bind_application_dispatch (application_prepare_t prepare,
                                    application_ready_t ready,
                                    stateful_ready_t stateful_ready = {});
    task_t<bool> notify_stateful_ready (const std::string &owner, stateful_take_t take);
    bool has_application_dispatch () const;
    void begin_application_receive_turn ();
    void end_application_receive_turn ();
    bool begin_application_drain (const std::string &owner);
    void end_application_drain (const std::string &owner);

    // admitted runs under the mailbox lock once the record is accepted and before any
    // claim can see it.
    bool try_enqueue (service_mailbox_record_t &&record,
                      const std::function<void ()> &admitted = {});
    std::shared_ptr<service_mailbox_claim_t> try_claim (service_mailbox_domain_t domain,
                                                        std::size_t message_budget,
                                                        std::size_t byte_budget);
    std::shared_ptr<service_mailbox_claim_t> try_claim_owner (service_mailbox_domain_t domain,
                                                              const std::string &owner,
                                                              std::size_t message_budget,
                                                              std::size_t byte_budget);
    using claim_handle_t = service_mailbox_slot_t;
    // Query preparation and original transfer share the existing producer boundary.
    template <class Prepare>
    auto prepare (const claim_handle_t &handle, Prepare prepare) const
      -> std::optional<std::invoke_result_t<Prepare, const service_mailbox_record_t &>>
    {
        std::lock_guard lock (_state->mutex);
        const auto *slot = slot_locked (handle);
        if (!slot)
            return std::nullopt;
        return prepare (*slot);
    }
    // One boundary step: a value from keep leaves the original in its slot for a later
    // take; otherwise the original transfers now. monostate means it is gone or not next.
    template <class Keep>
    auto take_unless (const claim_handle_t &handle, Keep keep)
      -> std::variant<
        std::monostate,
        service_mailbox_record_t,
        typename std::invoke_result_t<Keep, const service_mailbox_record_t &>::value_type>
    {
        std::lock_guard lock (_state->mutex);
        auto *slot = slot_locked (handle);
        if (!slot)
            return std::monostate{};
        if (auto kept = keep (std::as_const (*slot)))
            return std::move (*kept);
        if (!next_locked (handle))
            return std::monostate{};
        return transfer_locked (*slot);
    }
    std::optional<service_mailbox_record_t> take (const claim_handle_t &handle);
    // Relocation flow §4.2: observe runs in the producer step that transfers the original,
    // so a seal cut of its owner is wholly before or after this take.
    template <class Observe>
    std::optional<service_mailbox_record_t> take (const claim_handle_t &handle, Observe observe)
    {
        std::lock_guard lock (_state->mutex);
        auto *slot = slot_locked (handle);
        if (!slot || !next_locked (handle))
            return std::nullopt;
        observe ();
        return transfer_locked (*slot);
    }
    // Relocation flow §4.2 producer cut: in one producer step, takes every original of the
    // owners not taken yet, claimed or not, in FIFO order, then runs cut. A claimed original
    // carries its claim credit. A record enqueued afterwards stays for the ordinary claim.
    // Nothing is taken and cut does not run when admits refuses one of the originals.
    using cut_admits_t = std::function<bool (const service_mailbox_record_t &)>;
    std::optional<std::vector<std::vector<service_mailbox_record_t>>>
    cut_owners (const std::vector<std::string> &owners,
                const cut_admits_t &admits,
                const std::function<void ()> &cut);
    // Settles every credit of the claim still outstanding under one lock. Originals not
    // yet taken are discarded.
    bool release (service_mailbox_claim_t &claim);
    void close ();

    std::size_t pending_messages (service_mailbox_domain_t domain) const;
    std::size_t pending_bytes (service_mailbox_domain_t domain) const;

  private:
    friend class service_mailbox_settlement_t;
    friend struct service_mailbox_claim_t;

    enum class owner_phase_t
    {
        idle,
        ready,
        draining,
        retained
    };

    // A claim is the front prefix of records; it leaves the FIFO when its last
    // credit settles.
    struct owner_queue_t
    {
        std::deque<service_mailbox_record_t> records;
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::size_t active_messages = 0;
        std::size_t active_bytes = 0;
        owner_phase_t phase = owner_phase_t::idle;
    };

    struct domain_t
    {
        std::unordered_map<std::string, owner_queue_t> owners;
        std::deque<std::string> ready;
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::size_t active_messages = 0;
        std::size_t active_bytes = 0;
    };

    domain_t &domain (service_mailbox_domain_t value);
    const domain_t &domain (service_mailbox_domain_t value) const;
    std::shared_ptr<service_mailbox_claim_t>
    claim_owner_locked (domain_t &source,
                        service_mailbox_domain_t domain_value,
                        const std::string &owner,
                        std::size_t message_budget,
                        std::size_t byte_budget);
    static std::size_t retained_bytes (const service_mailbox_record_t &record);
    static service_mailbox_record_t *slot_locked (const claim_handle_t &handle);
    static bool next_locked (const claim_handle_t &handle);
    static service_mailbox_record_t transfer_locked (service_mailbox_record_t &slot);

    void notify_application_ready ();

    struct state_t
    {
        mutable std::mutex mutex;
        domain_t application;
        domain_t infrastructure;
        bool closed = false;
        application_prepare_t application_prepare;
        application_ready_t application_ready;
        stateful_ready_t stateful_ready;
        bool application_receive_turn = false;
    };
    static bool settle (const service_mailbox_settlement_t &settlement);
    static bool settle_once (service_mailbox_claim_t &claim, std::size_t index) noexcept;
    static bool settles_last (service_mailbox_claim_t &claim, std::size_t count) noexcept;
    static bool finish_locked (state_t &state, const service_mailbox_claim_t &claim);
    static void notify_application_ready_owned (const std::shared_ptr<state_t> &state);
    std::shared_ptr<state_t> _state = std::make_shared<state_t> ();
};

// One claim of an owner's FIFO prefix. records views the claimed slots until the
// claim settles; read them only before their take. Each slot credit settles once.
struct service_mailbox_claim_t
{
    std::string owner;
    service_mailbox_domain_t domain;
    std::vector<service_mailbox_record_t *> records;
    std::vector<std::size_t> record_bytes;
    std::size_t claimed_messages = 0;
    std::size_t claimed_bytes = 0;

    // Slot credits not yet settled.
    std::size_t outstanding () const noexcept
    {
        return _remaining.load (std::memory_order_acquire);
    }

  private:
    friend class service_mailbox_t;
    std::weak_ptr<service_mailbox_t::state_t> _mailbox;
    std::unique_ptr<std::atomic_bool[]> _settled;
    std::atomic_size_t _remaining{0};
};

inline bool service_mailbox_settlement_t::operator() () const
{
    return _claim && service_mailbox_t::settle (*this);
}

inline std::string service_mailbox_t::application_owner (const host::ready_record_t &owner)
{
    switch (owner.owner_kind) {
        case host::owner_kind_t::node:
            return application_owner (owner.owner_kind);
        case host::owner_kind_t::channel:
            return application_owner (owner.owner_kind, owner.channel_name);
        case host::owner_kind_t::spot:
            return application_owner (owner.owner_kind, owner.spot_id);
        case host::owner_kind_t::actor:
            if (owner.actor)
                return application_owner (owner.owner_kind, owner.actor->actor_id ().value ());
            throw std::invalid_argument ("application actor owner requires its reference");
    }
    throw std::invalid_argument ("unknown application owner kind");
}

} // namespace zlink::framework::runtime::mesh
