/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/operations/call_id.hpp"
#include "runtime/foundation/operation_terminal.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace zlink::framework
{
struct location_owner_token_t;
}

namespace zlink::framework::runtime::foundation
{

using call_id_t = runtime::call_id_t;
struct operation_completion_item_t;
struct operation_registry_drain_state_t;

class operation_registry_t
{
  public:
    using clock_t = std::chrono::steady_clock;
    using callback_t = std::function<void (operation_terminal_t, zlink::message_t)>;
    using before_dispatch_t = std::function<void ()>;
    // The management budget reads this clock; tests inject a deterministic one.
    using now_t = clock_t::time_point (*) ();

    struct request_identity_t
    {
        call_id_t operation;
        std::shared_ptr<const location_owner_token_t> lease;
    };
    std::optional<request_identity_t> request_identity (call_id_t id, call_id_t operation) const;

    operation_registry_t ();
    explicit operation_registry_t (now_t now);
    ~operation_registry_t () noexcept;

    bool register_operation (call_id_t id,
                             clock_t::time_point deadline,
                             callback_t callback,
                             std::vector<std::uint8_t> target_routing_id = {},
                             mesh_request_metric_t request_metric = {},
                             std::optional<request_identity_t> request = {});
    bool contains (const call_id_t &id) const;
    std::size_t fail_target (const std::vector<std::uint8_t> &target_routing_id,
                             operation_terminal_t terminal);
    bool complete (const call_id_t &id, zlink::message_t payload);
    // Packed infrastructure replies retain their delivery terminal and payload
    // for the caller to decode; request_terminal observes their semantic result.
    bool complete (const call_id_t &id,
                   zlink::message_t payload,
                   before_dispatch_t before_dispatch,
                   operation_terminal_t request_terminal = operation_terminal_t::completed);
    bool complete_async (const call_id_t &id, std::function<task_t<void> ()> before_dispatch);
    bool cancel (const call_id_t &id);
    bool fail (const call_id_t &id, operation_terminal_t terminal, zlink::message_t payload = {});
    bool fail (const call_id_t &id,
               operation_terminal_t terminal,
               zlink::message_t payload,
               before_dispatch_t before_dispatch,
               std::optional<operation_terminal_t> request_terminal = {});
    bool unregister (const call_id_t &id);
    std::size_t expire (clock_t::time_point now);
    // The next occupied management tick includes slots with remaining rounds.
    std::optional<clock_t::time_point> next_deadline () const;
    std::size_t shutdown ();
    std::size_t size () const;

  private:
    using id_hash_t = runtime::call_id_hash_t;

    static constexpr std::size_t deadline_slot_count = 256;
    static constexpr unsigned deadline_bitmap_word_bits = 64;
    static constexpr auto deadline_tick_duration = std::chrono::milliseconds (1);

    struct pending_t
    {
        std::unique_ptr<operation_completion_item_t> completion;
        std::vector<std::uint8_t> target_routing_id;
        call_id_t id;
        std::uint64_t deadline_rounds = 0;
        std::size_t deadline_slot = 0;
        pending_t *deadline_previous = nullptr;
        pending_t *deadline_next = nullptr;
        std::optional<request_identity_t> request;
    };

    bool take (const call_id_t &id, std::unique_ptr<operation_completion_item_t> &completion);
    std::optional<std::uint64_t> next_occupied_tick () const noexcept;
    void index_deadline (const call_id_t &id, clock_t::time_point deadline, pending_t &pending);
    void unindex_deadline (pending_t &pending) noexcept;

    std::shared_ptr<operation_registry_drain_state_t> _drain_state;
    mutable std::mutex _mutex;
    std::unordered_map<call_id_t, pending_t, id_hash_t> _pending;
    const now_t _now;
    const clock_t::time_point _deadline_origin;
    std::array<pending_t *, deadline_slot_count> _deadline_heads{};
    std::array<pending_t *, deadline_slot_count> _deadline_tails{};
    std::array<std::uint64_t, deadline_slot_count / deadline_bitmap_word_bits> _deadline_occupied{};
    std::uint64_t _deadline_tick = 0;
    pending_t *_deadline_cursor = nullptr;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::foundation
