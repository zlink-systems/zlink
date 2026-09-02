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
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zlink::detail
{

class completion_entry_t : public std::enable_shared_from_this<completion_entry_t>
{
  public:
    enum class kind_t { send, request };

    explicit completion_entry_t (
      std::shared_ptr<async_operation_state_t<void>> send_result_);
    explicit completion_entry_t (
      std::shared_ptr<async_operation_state_t<std::vector<message_t>>> request_result_);

    completion_entry_t (const completion_entry_t &) = delete;
    completion_entry_t &operator= (const completion_entry_t &) = delete;

    void publish (uint64_t completion_id_) noexcept;
    void fail_submit () noexcept;
    void capture (zlink_completion_t &completion_) noexcept;
    void wait_settled () noexcept;
    std::vector<message_t> wait_request ();
    kind_t kind () const noexcept { return _kind; }

  private:
    void settle_if_joined (std::unique_lock<std::mutex> &lock_) noexcept;

    kind_t _kind;
    std::shared_ptr<async_operation_state_t<void>> _send_result;
    std::shared_ptr<async_operation_state_t<std::vector<message_t>>> _request_result;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<message_t> _reply_parts;
    std::exception_ptr _failure;
    uint64_t _completion_id = 0;
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
    void unregister_entry (completion_entry_t *entry_) noexcept;
    size_t drain (bool wait_for_publish_);

    void transfer_to_public (const void *poller_owner_);
    void transfer_to_runtime (const void *poller_owner_) noexcept;
    void shutdown () noexcept;

  private:
    void start_runtime_owner_locked ();
    void stop_runtime_owner_locked (std::unique_lock<std::mutex> &lock_) noexcept;
    void runtime_loop () noexcept;

    void *_socket;
    void *_runtime_poller = nullptr;
    std::mutex _mutex;
    std::unordered_map<void *, std::shared_ptr<completion_entry_t>> _entries;
    const void *_public_owner = nullptr;
    std::thread _runtime_thread;
    bool _runtime_stop = false;
    bool _shutdown = false;
};

} // namespace zlink::detail

#endif
