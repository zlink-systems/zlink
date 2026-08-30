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
#include <utility>
#include <vector>

#include "api/socket/request_completion_queue_internal.hpp"
#include "api/socket/request_reply_runtime_core.hpp"
#include "api/socket/request_timeout_scheduler_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/ctx_physical_queue_registry.hpp"

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

struct pending_request_identity_t
{
    pending_request_identity_t () : request_seq (0), cookie (0) {}

    uint64_t request_seq;
    uint64_t cookie;

    bool operator== (const pending_request_identity_t &other_) const
    {
        return request_seq == other_.request_seq && cookie == other_.cookie;
    }
};

struct pending_request_token_t
{
    pending_request_token_t () : resolved_timeout_ms (0) {}

    pending_request_identity_t identity;
    //  Registration resolves the policy once. The token carries that decision
    //  across physical admission so arm does not reopen the pending aggregate.
    uint32_t resolved_timeout_ms;
};

struct pending_request_t
{
    pending_request_identity_t identity;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    //  Retained by the lifecycle owner so a resumed multipart send can rebuild
    //  its ephemeral arm token without reinterpreting the current socket policy.
    uint32_t resolved_timeout_ms;
    zlink_reply_handler_fn handler;
    void *userdata;
    std::shared_ptr<zlink::request_timeout::task_t> timeout_task;
};

struct dealer_reply_target_t
{
    dealer_reply_target_t ();

    zlink::pipe_t *pipe;
    uint64_t request_seq;
    bool checked_out;
};

struct router_reply_target_t
{
    router_reply_target_t ();

    zlink::pipe_t *pipe;
    bool checked_out;
};

struct socket_request_reply_state_t : public zlink::request_reply_runtime::sequence_state_t
{
    explicit socket_request_reply_state_t (zlink::socket_base_t *socket_, int socket_type_);

    zlink::socket_base_t *socket;
    int socket_type;
    std::mutex mutex;
    // Request sequences select the aggregate on the wire. The independent
    // socket-owned cookie fences local observers, timeout callbacks and send
    // failures after a forced sequence wrap/reuse.
    uint64_t next_pending_cookie;
    std::unordered_map<uint64_t, pending_request_t> pending_requests;
    std::unordered_map<uint64_t, dealer_reply_target_t> dealer_reply_targets;
    std::unordered_map<pending_key_t, router_reply_target_t, pending_key_hash_t>
      router_reply_targets;
    size_t reply_target_slots;
    size_t reply_target_reservations;
    size_t reply_target_checkouts;
    uint64_t dealer_next_reply_token;
    bool closing;
    zlink::request_completion::queue_state_t completion;
};

struct router_recv_metadata_tls_t
{
    zlink_routing_id_t source_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
};

router_recv_metadata_tls_t &router_recv_metadata_tls ();

uint64_t allocate_dealer_reply_token (socket_request_reply_state_t *state_);
int recv_router_message_direct (const socket_handle_t &handle_,
                                const zlink_routing_id_t **source_node_rid_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_,
                                zlink_msg_t *terminal_part_out_ = NULL,
                                bool *terminal_part_returned_out_ = NULL,
                                zlink_routing_id_t *terminal_source_storage_ = NULL);
int recv_dealer_message_direct (const socket_handle_t &handle_,
                                const std::shared_ptr<socket_request_reply_state_t> &state_,
                                bool typed_receive_,
                                uint8_t *message_type_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_,
                                zlink_msg_t *terminal_part_out_ = NULL,
                                bool *terminal_part_returned_out_ = NULL);
int take_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                              uint64_t request_token_,
                              dealer_reply_target_t *target_out_);
void restore_dealer_reply_target (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                  uint64_t request_token_);
void commit_dealer_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t request_token_);
void revoke_dealer_reply_target (const socket_handle_t &handle_,
                                 uint64_t request_token_);
void forget_dealer_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_);
bool take_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  zlink::pipe_t **application_pipe_out_);
void restore_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_);
void commit_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_);
void revoke_router_reply_target (const socket_handle_t &handle_,
                                 const zlink_routing_id_t *peer_rid_,
                                 uint64_t request_seq_);
void forget_router_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_);
int send_request_reply_message (const socket_handle_t &handle_,
                                const zlink_routing_id_t *peer_rid_,
                                zlink_msg_t *staged_parts_,
                                size_t staged_part_count_,
                                zlink_msg_t *final_part_,
                                zlink_send_flags_t flags_,
                                uint8_t message_type_,
                                uint64_t request_seq_);
int send_completion_staged_frames (zlink::socket_base_t *socket_,
                                   zlink::pipe_t *application_pipe_,
                                   const zlink_routing_id_t *peer_rid_,
                                   zlink_msg_t *staged_parts_,
                                   size_t staged_part_count_,
                                   zlink_msg_t *final_part_);
std::shared_ptr<socket_request_reply_state_t>
find_or_create_request_reply_state (const socket_handle_t &handle_);
std::shared_ptr<socket_request_reply_state_t>
find_request_reply_state (const socket_handle_t &handle_);
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
int add_socket_pending_request_locked (socket_request_reply_state_t *state_,
                                       pending_request_t pending_);
bool remove_socket_pending_request_locked (socket_request_reply_state_t *state_,
                                           const pending_request_identity_t &identity_,
                                           pending_request_t *pending_out_);
bool take_pending_reply_from_transport_locked (
  socket_request_reply_state_t *state_,
  uint64_t request_seq_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  pending_request_t *pending_out_);
bool take_disconnected_socket_pending_request_locked (
  socket_request_reply_state_t *state_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  pending_request_t *pending_out_);
bool take_next_socket_pending_request_locked (
  socket_request_reply_state_t *state_, pending_request_t *pending_out_);

bool remove_socket_pending_request (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                    const pending_request_identity_t &identity_,
                                    pending_request_t *pending_out_);
int schedule_socket_pending_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_request_identity_t &identity_,
  uint32_t timeout_ms_,
  std::shared_ptr<zlink::request_timeout::task_t> *task_out_);
int arm_socket_pending_request_timeout (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_request_token_t &token_);
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
int drain_close_request_reply_socket (const socket_handle_t &handle_);
void cleanup_request_reply_socket (const socket_handle_t &handle_);

#ifdef ZLINK_BUILD_TESTS
enum request_reply_allocation_failpoint_t
{
    request_reply_allocation_none = 0,
    request_reply_allocation_stage_payload,
    request_reply_allocation_reply_key,
    request_reply_allocation_pending_insert,
    request_reply_allocation_lazy_state_create,
    request_reply_allocation_receive_spill,
    request_reply_allocation_payload_export,
    request_reply_allocation_receive_part_stage
};

typedef void (*request_reply_timeout_after_remove_hook_fn) (void *userdata_);

void test_set_request_reply_allocation_failpoint (
  request_reply_allocation_failpoint_t failpoint_);
void test_throw_request_reply_allocation_failpoint (
  request_reply_allocation_failpoint_t failpoint_);
void test_set_request_reply_timeout_after_remove_hook (
  request_reply_timeout_after_remove_hook_fn hook_, void *userdata_);
#endif
}
}

#endif
