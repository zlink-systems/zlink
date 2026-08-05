/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "api/socket/request_completion_queue_internal.hpp"
#include "api/socket/request_reply_runtime_core.hpp"
#include "api/socket/request_timeout_scheduler_internal.hpp"
#include "api/socket/socket_api_internal.hpp"

namespace zlink
{
class pipe_t;
class socket_base_t;

namespace socket_reqrep_internal
{
static const size_t max_reply_target_slots = 65536;

void process_completion_pipe (zlink::socket_base_t *socket_, zlink::pipe_t *pipe_);
struct pending_key_t
{
    std::string peer_rid;
    uint64_t request_seq;

    bool operator== (const pending_key_t &other_) const;
    bool operator< (const pending_key_t &other_) const;
};

struct pending_key_hash_t
{
    size_t operator() (const pending_key_t &key_) const;
};

struct pending_request_t
{
    pending_key_t key;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    zlink_reply_handler_fn handler;
    void *userdata;
    std::shared_ptr<zlink::request_timeout::task_t> timeout_task;
};

struct dealer_reply_target_t
{
    dealer_reply_target_t ();

    zlink::pipe_t *pipe;
    uint64_t request_seq;
};

struct socket_request_reply_state_t : public zlink::request_reply_runtime::sequence_state_t
{
    explicit socket_request_reply_state_t (zlink::socket_base_t *socket_, int socket_type_);

    zlink::socket_base_t *socket;
    int socket_type;
    std::mutex mutex;
    std::unordered_map<pending_key_t, pending_request_t, pending_key_hash_t> pending_requests;
    std::unordered_map<uint64_t, pending_key_t> pending_request_keys_by_seq;
    std::unordered_map<uint64_t, dealer_reply_target_t> dealer_reply_targets;
    std::unordered_map<pending_key_t, zlink::pipe_t *, pending_key_hash_t>
      router_reply_targets;
    size_t reply_target_slots;
    uint64_t dealer_next_reply_token;
    bool closing;
    zlink_completion_control_handler_fn completion_control_handler;
    void *completion_control_userdata;
    zlink::request_completion::queue_state_t completion;
};

struct router_recv_metadata_tls_t
{
    zlink_routing_id_t source_rid;
};

router_recv_metadata_tls_t &router_recv_metadata_tls ();

int validate_request_parts (zlink_msg_t *parts_, size_t part_count_);
uint64_t allocate_dealer_reply_token (socket_request_reply_state_t *state_);
int recv_router_message_direct (socket_handle_t handle_,
                                const zlink_routing_id_t **source_node_rid_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_);
int recv_dealer_message_direct (socket_handle_t handle_,
                                const std::shared_ptr<socket_request_reply_state_t> &state_,
                                uint8_t *message_type_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_);
int take_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                              uint64_t request_token_,
                              dealer_reply_target_t *target_out_);
void restore_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                  uint64_t request_token_,
                                  const dealer_reply_target_t &target_);
bool take_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  zlink::pipe_t **application_pipe_out_);
void restore_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  zlink::pipe_t *application_pipe_);
void forget_router_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_);
void release_reply_target_slot (
  const std::shared_ptr<socket_request_reply_state_t> &state_);
int send_request_reply_message (void *socket_handle_,
                                const zlink_routing_id_t *peer_rid_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                zlink_send_flags_t flags_,
                                uint8_t message_type_,
                                uint64_t request_seq_);
int send_completion_frames (zlink::socket_base_t *socket_,
                            zlink::pipe_t *application_pipe_,
                            const zlink_routing_id_t *peer_rid_,
                            zlink_msg_t *parts_,
                            size_t part_count_);
std::shared_ptr<socket_request_reply_state_t>
find_or_create_request_reply_state (socket_handle_t handle_);
std::shared_ptr<socket_request_reply_state_t> find_request_reply_state (socket_handle_t handle_);
int ensure_completion_queue_ready (const std::shared_ptr<socket_request_reply_state_t> &state_);
int queue_reply_completion (const std::shared_ptr<socket_request_reply_state_t> &state_,
                            zlink_reply_handler_fn handler_,
                            void *userdata_,
                            int errnum_,
                            zlink_msg_t *parts_,
                            size_t part_count_);
int drain_reply_completions (const std::shared_ptr<socket_request_reply_state_t> &state_,
                             void *owner_handle_);
int drain_reply_completions_while_closing (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  void *owner_handle_);
bool has_pending_reply_completions (const std::shared_ptr<socket_request_reply_state_t> &state_);
void claim_completion_owner (const std::shared_ptr<socket_request_reply_state_t> &state_);
bool current_thread_is_completion_owner (
  const std::shared_ptr<socket_request_reply_state_t> &state_);
bool in_socket_request_completion_callback (void *socket_);
inline void add_socket_pending_request_locked (socket_request_reply_state_t *state_,
                                               const pending_key_t &key_,
                                               const pending_request_t &pending_)
{
    if (!state_)
        return;

    state_->pending_sequences.insert (key_.request_seq);
    state_->pending_requests[key_] = pending_;
    state_->pending_request_keys_by_seq[key_.request_seq] = key_;
}

inline bool remove_socket_pending_request_locked (socket_request_reply_state_t *state_,
                                                  const pending_key_t &key_,
                                                  pending_request_t *pending_out_)
{
    if (!state_)
        return false;

    std::unordered_map<pending_key_t, pending_request_t, pending_key_hash_t>::iterator it =
      state_->pending_requests.find (key_);
    if (it == state_->pending_requests.end ())
        return false;

    if (pending_out_)
        *pending_out_ = it->second;
    state_->pending_sequences.erase (it->first.request_seq);
    state_->pending_request_keys_by_seq.erase (it->first.request_seq);
    state_->pending_requests.erase (it);
    return true;
}

inline bool take_pending_reply_from_transport_locked (
  socket_request_reply_state_t *state_,
  const pending_key_t &key_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  pending_request_t *pending_out_)
{
    if (!state_)
        return false;

    pending_key_t key = key_;
    std::unordered_map<pending_key_t, pending_request_t,
                       pending_key_hash_t>::const_iterator pending_it =
      state_->pending_requests.find (key);
    if (pending_it == state_->pending_requests.end ()) {
        //  A request may be addressed to a routing id the caller only knows
        //  by intent, while the reply carries the peer's settled routing id.
        //  The sequence is allocated per socket, so it identifies the request
        //  on its own; the transport pair below still fences a reply that
        //  belongs to an earlier connection.
        std::unordered_map<uint64_t, pending_key_t>::const_iterator by_seq =
          state_->pending_request_keys_by_seq.find (key_.request_seq);
        if (by_seq == state_->pending_request_keys_by_seq.end ())
            return false;
        key = by_seq->second;
        pending_it = state_->pending_requests.find (key);
        if (pending_it == state_->pending_requests.end ())
            return false;
    }
    if (pending_it->second.transport_pair_id != transport_pair_id_
        || pending_it->second.transport_pair_generation
             != transport_pair_generation_)
        return false;
    return remove_socket_pending_request_locked (state_, key, pending_out_);
}

bool remove_socket_pending_request (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                    const pending_key_t &key_,
                                    pending_request_t *pending_out_);
int schedule_socket_pending_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  uint32_t timeout_ms_,
  std::shared_ptr<zlink::request_timeout::task_t> *task_out_);
void queue_socket_pending_timeout_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_, const pending_request_t &pending_);
bool has_pending_request_work (const std::shared_ptr<socket_request_reply_state_t> &state_);
void fail_disconnected_peer_requests (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  int errnum_);
int drain_close_request_reply_socket (socket_handle_t handle_);
void cleanup_request_reply_socket (socket_handle_t handle_);
}
}

#endif
