/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_COMPLETION_OWNER_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_COMPLETION_OWNER_HPP_INCLUDED

#include "async_operation_state.hpp"

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Errors/results.hpp>
#include <zlink.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zlink::detail
{

struct operation_state_t;

class completion_entry_t : public std::enable_shared_from_this<completion_entry_t>
{
  public:
    enum class kind_t { send_retry, request };

    explicit completion_entry_t (
      async_operation_state_t<void> *send_result_,
      std::unique_ptr<operation_state_t> send_operation_);
    explicit completion_entry_t (
      async_operation_state_t<std::vector<message_t>> *request_result_);
    explicit completion_entry_t (
      const std::shared_ptr<async_operation_state_t<std::vector<message_t>>> &
        request_result_);
    completion_entry_t (
      async_operation_state_t<std::vector<message_t>> *request_result_,
      std::unique_ptr<operation_state_t> request_operation_);
    ~completion_entry_t ();

    completion_entry_t (const completion_entry_t &) = delete;
    completion_entry_t &operator= (const completion_entry_t &) = delete;

    bool start_send (bool defer_source_detach_ = false);
    void detach_send_sources () noexcept;
    void start_request ();
    void publish (uint64_t completion_id_) noexcept;
    void fail_submit () noexcept;
    bool capture (zlink_completion_t &completion_) noexcept;
    void terminate (int terminal_errno_) noexcept;
    void wait_settled () noexcept;
    std::vector<message_t> wait_request ();
    kind_t kind () const noexcept { return _kind; }

  private:
    bool submit_send_attempt (bool initial_, bool defer_source_detach_ = false);
    bool submit_request_attempt (bool initial_);
    void fail_send (std::exception_ptr failure_) noexcept;
    void fail_request (std::exception_ptr failure_) noexcept;
    void settle_if_joined (std::unique_lock<std::mutex> &lock_) noexcept;

    kind_t _kind;
    async_operation_state_t<void> *_send_result;
    std::unique_ptr<operation_state_t> _send_operation;
    async_operation_state_t<std::vector<message_t>> *_request_result;
    std::unique_ptr<operation_state_t> _request_operation;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<message_t> _reply_parts;
    std::exception_ptr _failure;
    uint64_t _completion_id = 0;
    bool _request_waiting_writable = false;
    bool _published = false;
    bool _captured = false;
    bool _settled = false;
};

class completion_owner_t : public std::enable_shared_from_this<completion_owner_t>
{
  public:
    explicit completion_owner_t (void *socket_);
    ~completion_owner_t ();

    completion_owner_t (const completion_owner_t &) = delete;
    completion_owner_t &operator= (const completion_owner_t &) = delete;

    void register_entry (const std::shared_ptr<completion_entry_t> &entry_);
    void register_send_entry (const std::shared_ptr<completion_entry_t> &entry_);
    void unregister_entry (completion_entry_t *entry_) noexcept;
    size_t drain (bool wait_for_publish_, uint64_t runtime_generation_ = 0);

    void transfer_to_public (const void *poller_owner_);
    void transfer_to_runtime (const void *poller_owner_) noexcept;
    void shutdown (int terminal_errno_ = ESHUTDOWN) noexcept;

  private:
    void start_runtime_owner_locked ();
    void stop_runtime_owner_locked (std::unique_lock<std::mutex> &lock_) noexcept;
    void runtime_loop (uint64_t runtime_generation_) noexcept;

    void *_socket;
    void *_runtime_poller = nullptr;
    std::mutex _mutex;
    // Entry identities are never pooled: Core may still carry one as callback
    // userdata until its exact completion is drained. Only the unordered-map
    // nodes are recycled under _mutex, avoiding one allocator round trip for
    // every request while preserving each entry's unique lifetime.
    std::pmr::unsynchronized_pool_resource _entry_map_pool;
    // The common one-outstanding-operation case stays out of the hash table.
    // Additional concurrent operations retain the existing PMR-backed map.
    std::shared_ptr<completion_entry_t> _inline_entry;
    std::pmr::unordered_map<void *, std::shared_ptr<completion_entry_t>> _entries;
    std::pmr::unordered_map<void *, zlink_completion_t> _early_send_completions;
    size_t _send_entry_count = 0;
    const void *_public_owner = nullptr;
    std::thread _runtime_thread;
    uint64_t _runtime_generation = 0;
    bool _runtime_stop = false;
    bool _shutdown = false;
};

} // namespace zlink::detail

#endif
