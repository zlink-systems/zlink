/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <zlink/framework/contracts/dispatch/request_task.hpp>

#include <span>

#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/mesh/service_liveness_registry.hpp"

#include <chrono>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string_view>

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/request_result.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include "runtime/eventing/runtime_wake_timer.hpp"

namespace zlink
{
class router_socket_t;
}

namespace zlink::framework::detail::backend
{

using raw_bytes_t = std::vector<std::uint8_t>;
using raw_message_t = std::vector<raw_bytes_t>;
using raw_send_stage_trace_t = std::function<void (std::string_view, std::string_view)>;

inline void encode_route_connection_id (std::uint64_t value, std::span<std::uint8_t> result)
{
    for (std::size_t index = 0; index < result.size (); ++index) {
        const auto shift = static_cast<unsigned int> ((result.size () - index - 1) * 8);
        result[index] = static_cast<std::uint8_t> ((value >> shift) & 0xffu);
    }
}

inline raw_bytes_t route_connection_id (std::uint64_t value)
{
    raw_bytes_t result (sizeof (value));
    encode_route_connection_id (value, result);
    return result;
}

inline bool is_route_connection (const raw_bytes_t &identity, std::uint64_t generation) noexcept
{
    if (identity.size () != sizeof (generation))
        return false;
    std::uint64_t value = 0;
    for (const auto byte : identity)
        value = (value << 8) | byte;
    return value == generation;
}

struct raw_received_t
{
    raw_bytes_t source_routing_id;
    std::optional<zlink::reply_token_t> reply_token;
    std::vector<zlink::message_t> parts;
    // Opaque nonzero generation of the Core selected route that delivered
    // this record (Core ROUTER §10.1); compared only for equality.
    std::uint64_t route_generation = 0;
};

enum class raw_request_failure_phase_t
{
    initial_admission,
    completion_terminal
};

struct raw_request_failure_t
{
    raw_request_failure_phase_t phase;
    std::optional<zlink::submit_result_t> submit_result;
    int internal_errno = 0;
};

struct raw_request_completion_t
{
    zlink::request_result_t terminal = zlink::request_result_t::internal_error;
    std::vector<zlink::message_t> parts;
    std::optional<raw_request_failure_t> failure;
};

// Raw request callbacks are a closed set of submit/terminal captures. Keep them
// in the existing observer frame; oversize captures fail at compile time.
class raw_request_callback_t
{
    using result_type = result_t<raw_request_completion_t>;
    struct operations_t
    {
        void (*invoke) (void *, result_type);
        void (*copy) (void *, const void *);
        void (*move) (void *, void *) noexcept;
        void (*destroy) (void *) noexcept;
    };

  public:
    raw_request_callback_t () = default;
    raw_request_callback_t (std::nullptr_t) noexcept {}
    template <typename TCallback>
        requires (!std::is_same_v<std::decay_t<TCallback>, raw_request_callback_t>)
    raw_request_callback_t (TCallback &&callback)
    {
        using callback_t = std::decay_t<TCallback>;
        static_assert (sizeof (callback_t) <= sizeof (_storage));
        static_assert (alignof (callback_t) <= alignof (std::max_align_t));
        static_assert (std::is_nothrow_move_constructible_v<callback_t>);
        static_assert (std::is_copy_constructible_v<callback_t>);
        static_assert (std::is_invocable_r_v<void, callback_t &, result_type>);
        if constexpr (std::is_same_v<callback_t, std::function<void (result_type)>>
                      || std::is_pointer_v<callback_t> || std::is_member_pointer_v<callback_t>) {
            if (!callback)
                return;
        }
        static constexpr operations_t operations{
          [] (void *data, result_type result) {
              std::invoke (*std::launder (reinterpret_cast<callback_t *> (data)),
                           std::move (result));
          },
          [] (void *destination, const void *source) {
              std::construct_at (reinterpret_cast<callback_t *> (destination),
                                 *std::launder (reinterpret_cast<const callback_t *> (source)));
          },
          [] (void *destination, void *source) noexcept {
              auto *original = std::launder (reinterpret_cast<callback_t *> (source));
              std::construct_at (reinterpret_cast<callback_t *> (destination),
                                 std::move (*original));
              std::destroy_at (original);
          },
          [] (void *data) noexcept {
              std::destroy_at (std::launder (reinterpret_cast<callback_t *> (data)));
          }};
        std::construct_at (reinterpret_cast<callback_t *> (_storage),
                           std::forward<TCallback> (callback));
        _operations = &operations;
    }
    raw_request_callback_t (const raw_request_callback_t &other)
    {
        if (other._operations) {
            other._operations->copy (_storage, other._storage);
            _operations = other._operations;
        }
    }
    raw_request_callback_t (raw_request_callback_t &&other) noexcept { *this = std::move (other); }
    raw_request_callback_t &operator= (const raw_request_callback_t &other)
    {
        if (this != &other) {
            raw_request_callback_t copy (other);
            *this = std::move (copy);
        }
        return *this;
    }
    raw_request_callback_t &operator= (raw_request_callback_t &&other) noexcept
    {
        if (this != &other) {
            reset ();
            if (other._operations) {
                const auto *operations = std::exchange (other._operations, nullptr);
                operations->move (_storage, other._storage);
                _operations = operations;
            }
        }
        return *this;
    }
    ~raw_request_callback_t () { reset (); }
    explicit operator bool () const noexcept { return _operations != nullptr; }
    void operator() (result_type result) const
    {
        if (!_operations)
            throw std::bad_function_call ();
        _operations->invoke (_storage, std::move (result));
    }

  private:
    void reset () noexcept
    {
        if (const auto *operations = std::exchange (_operations, nullptr))
            operations->destroy (_storage);
    }
    alignas (std::max_align_t) mutable std::byte _storage[128];
    const operations_t *_operations = nullptr;
};

// A send that the binding accepts immediately has its terminal admission
// result now.  Only a DONTWAIT rejection needs the binding-owned completion
// task that retries on writable readiness.
enum class raw_send_submission_state_t
{
    immediate,
    pending_backpressure
};

struct raw_send_submission_t
{
    raw_send_submission_state_t state;
    std::optional<result_t<zlink::submit_result_t>> immediate_result;
    std::shared_ptr<task_t<zlink::submit_result_t>> pending_completion;
};

class raw_route_port_t
{
  public:
    explicit raw_route_port_t (
      zlink::router_socket_t &socket,
      std::shared_ptr<std::mutex> shared_socket_mutex = {},
      zlink::poll_event_flag_t receive_events = zlink::poll_event_flag_t::pollin,
      zlink::poller_t *shared_poller = nullptr,
      std::uintptr_t poller_slot = 1);

    raw_send_submission_t submit_send (const raw_bytes_t &target_routing_id,
                                       raw_message_t parts,
                                       raw_send_stage_trace_t trace = {});
    raw_send_submission_t submit_send (const raw_bytes_t &target_routing_id,
                                       std::vector<zlink::message_t> parts,
                                       raw_send_stage_trace_t trace = {});
    task_t<zlink::submit_result_t> send_result (const raw_bytes_t &target_routing_id,
                                                raw_message_t parts,
                                                raw_send_stage_trace_t trace = {});
    task_t<bool> send (const raw_bytes_t &target_routing_id, raw_message_t parts);
    void set_request_receive_source (
      std::function<runtime::mesh::service_liveness_registry_t::connection_t (const raw_bytes_t &)>
        source)
    {
        _request_receive_source = std::move (source);
    }
    void request (std::span<const std::uint8_t> target_routing_id,
                  raw_message_t &&parts,
                  std::chrono::milliseconds timeout,
                  raw_request_callback_t completion,
                  runtime::mesh::service_liveness_registry_t::connection_t admission = {},
                  std::shared_ptr<task_completion_source_t<void>> admitted = {});
    void request (std::span<const std::uint8_t> target_routing_id,
                  std::vector<zlink::message_t> &&parts,
                  std::chrono::milliseconds timeout,
                  raw_request_callback_t completion,
                  runtime::mesh::service_liveness_registry_t::connection_t admission = {},
                  std::shared_ptr<task_completion_source_t<void>> admitted = {});
    task_t<raw_request_completion_t>
    request (const raw_bytes_t &target_routing_id,
             raw_message_t parts,
             std::chrono::milliseconds timeout,
             runtime::mesh::service_liveness_registry_t::connection_t admission = {},
             std::shared_ptr<task_completion_source_t<void>> admitted = {});
    std::optional<zlink::poll_event_flag_t> poll (std::chrono::milliseconds timeout,
                                                  bool accept_application_receive = true);
    void signal_activity () noexcept;
    std::optional<raw_received_t> receive_if_ready (zlink::poll_event_flag_t revents);
    std::optional<raw_received_t> try_receive ();
    bool reply (const raw_received_t &request, raw_message_t parts);
    bool reply (const raw_received_t &request, std::vector<zlink::message_t> messages);
    void close ();

  private:
    template <typename Parts>
    raw_send_submission_t submit_send_impl (const raw_bytes_t &target_routing_id,
                                            Parts parts,
                                            raw_send_stage_trace_t trace);
    template <typename Parts>
    void request_impl (std::span<const std::uint8_t> target_routing_id,
                       Parts &&parts,
                       std::chrono::milliseconds timeout,
                       raw_request_callback_t completion,
                       runtime::mesh::service_liveness_registry_t::connection_t admission,
                       std::shared_ptr<task_completion_source_t<void>> admitted);

    std::function<runtime::mesh::service_liveness_registry_t::connection_t (const raw_bytes_t &)>
      _request_receive_source;
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller;
    std::uintptr_t _poller_slot;
    zlink::router_socket_t *_socket;
    // Poller registration/wait/close have their own resource lifetime. A
    // blocking wait must not hold the independent socket submission gate.
    std::mutex _poller_mutex;
    std::shared_ptr<std::mutex> _socket_mutex;
    zlink::poll_event_flag_t _receive_events;
    runtime::eventing::runtime_wake_timer_t _wake_timer;
    // Keep the binding receive envelope across polls so its message storage
    // capacity is reused by the binding public receive API.
    zlink::received_t _received;
};

} // namespace zlink::framework::detail::backend
