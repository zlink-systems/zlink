/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "api/socket/request_reply_runtime_core.hpp"
#include "api/socket/request_timeout_scheduler_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_completion_queue_internal.hpp"

namespace zlink
{
class pipe_t;
class socket_base_t;
enum pipe_message_admission_t : int;

namespace socket_reqrep_internal
{
static const size_t max_reply_target_slots = 65536;
//  Bound one completion-pipe owner turn without cutting a multipart record.
//  A whole-record budget trades a small amount of single-pipe throughput for
//  bounded latency across ready transport pairs.
static const size_t completion_pipe_record_budget = 64;

enum completion_pipe_drain_result_t
{
    completion_pipe_drained,
    completion_pipe_public_head,
    completion_pipe_budget_exhausted,
    completion_pipe_terminated
};

completion_pipe_drain_result_t process_completion_pipe (
  zlink::socket_base_t *socket_, zlink::pipe_t *pipe_);

// Public routing ids are bounded by zlink_routing_id_t. Keep reply-token keys
// inline so a normal 16-byte RID never allocates a std::string on receive,
// checkout, validation, or erase.
struct fixed_routing_id_key_t
{
    fixed_routing_id_key_t ();
    fixed_routing_id_key_t (const fixed_routing_id_key_t &other_);
    fixed_routing_id_key_t &operator= (
      const fixed_routing_id_key_t &other_);

    void assign (const void *data_, size_t size_);
    bool empty () const;
    size_t size () const;
    const unsigned char *data () const;
    size_t hash () const;
    bool operator== (const fixed_routing_id_key_t &other_) const;

  private:
    uint8_t _size;
    unsigned char _data[255];
};

struct fixed_routing_id_key_hash_t
{
    size_t operator() (const fixed_routing_id_key_t &key_) const;
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

struct request_correlation_lease_t
{
    request_correlation_lease_t ();
    ~request_correlation_lease_t ();
    request_correlation_lease_t (request_correlation_lease_t &&other_) noexcept;
    request_correlation_lease_t &operator= (
      request_correlation_lease_t &&other_) noexcept;
    request_correlation_lease_t (const request_correlation_lease_t &) = delete;
    request_correlation_lease_t &operator= (
      const request_correlation_lease_t &) = delete;

    void adopt (zlink::pipe_t *pipe_, uint64_t accounted_bytes_);
    void release ();
    zlink::pipe_t *pipe () const;

  private:
    zlink::pipe_t *_pipe;
    uint64_t _accounted_bytes;
};

struct pending_request_t
{
    pending_request_t ();
    pending_request_t (pending_request_t &&) noexcept = default;
    pending_request_t &operator= (pending_request_t &&) noexcept = default;
    pending_request_t (const pending_request_t &) = delete;
    pending_request_t &operator= (const pending_request_t &) = delete;

    pending_request_identity_t identity;
    // Explicit endpoint/RID removal resolves by logical owner; handover and
    // the stale reply fence use the submit-time physical pair identity.
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    request_correlation_lease_t correlation;
    //  Retained by the lifecycle owner so a resumed multipart send can rebuild
    //  its ephemeral arm token without reinterpreting the current socket policy.
    uint32_t resolved_timeout_ms;
    zlink::socket_completion::reservation_t *pull_completion;
    //  Zero until physical admission commits. Armed requests retain only
    //  their absolute deadline; one socket-owned task covers the aggregate.
    uint64_t timeout_deadline_ns;
};

// Socket-owned intrusive request index. The common 64 outstanding records
// live inline; higher concurrency grows in fixed slabs that remain available
// for the socket lifetime. Hash buckets and live iteration are intrusive, so
// steady request admission/removal never allocates a map node.
class pending_request_store_t
{
  public:
    struct node_t
    {
        node_t ();

        uint64_t first;
        pending_request_t second;
        node_t *bucket_next;
        node_t *live_previous;
        node_t *live_next;
        node_t *free_next;
    };

    class const_iterator;
    class iterator
    {
      public:
        iterator (node_t *node_ = NULL) : _node (node_) {}
        node_t &operator* () const { return *_node; }
        node_t *operator-> () const { return _node; }
        iterator &operator++ ()
        {
            _node = _node ? _node->live_next : NULL;
            return *this;
        }
        bool operator== (const iterator &other_) const
        {
            return _node == other_._node;
        }
        bool operator!= (const iterator &other_) const
        {
            return !(*this == other_);
        }

      private:
        friend class pending_request_store_t;
        friend class const_iterator;
        node_t *_node;
    };

    class const_iterator
    {
      public:
        const_iterator (const node_t *node_ = NULL) : _node (node_) {}
        const_iterator (const iterator &other_) : _node (other_._node) {}
        const node_t &operator* () const { return *_node; }
        const node_t *operator-> () const { return _node; }
        const_iterator &operator++ ()
        {
            _node = _node ? _node->live_next : NULL;
            return *this;
        }
        bool operator== (const const_iterator &other_) const
        {
            return _node == other_._node;
        }
        bool operator!= (const const_iterator &other_) const
        {
            return !(*this == other_);
        }

      private:
        friend class pending_request_store_t;
        const node_t *_node;
    };

    pending_request_store_t ();
    ~pending_request_store_t ();

    std::pair<iterator, bool> emplace (uint64_t request_seq_,
                                      pending_request_t &&pending_);
    iterator find (uint64_t request_seq_);
    const_iterator find (uint64_t request_seq_) const;
    size_t count (uint64_t request_seq_) const;
    iterator begin () { return iterator (_live_head); }
    const_iterator begin () const { return const_iterator (_live_head); }
    iterator end () { return iterator (); }
    const_iterator end () const { return const_iterator (); }
    iterator erase (iterator position_);
    bool empty () const { return _size == 0; }
    size_t size () const { return _size; }

  private:
    enum
    {
        inline_node_count = 64,
        slab_node_count = 64,
        bucket_count = 1024
    };
    struct slab_t
    {
        slab_t () : next (NULL) {}
        node_t nodes[slab_node_count];
        slab_t *next;
    };

    static size_t bucket_for (uint64_t request_seq_);
    void add_free_nodes (node_t *nodes_, size_t count_);
    bool grow ();

    node_t *_buckets[bucket_count];
    node_t _inline_nodes[inline_node_count];
    node_t *_free_head;
    node_t *_live_head;
    node_t *_live_tail;
    slab_t *_slabs;
    size_t _size;
    size_t _capacity;

    pending_request_store_t (const pending_request_store_t &) = delete;
    pending_request_store_t &operator= (
      const pending_request_store_t &) = delete;
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

    fixed_routing_id_key_t peer_rid;
    zlink::pipe_t *pipe;
    zlink::pipe_t *source_pipe_identity;
    int source_peer_socket_type;
    uint64_t wire_request_seq;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    uint64_t route_binding_token;
    bool checked_out;
    bool revoked;
};

struct router_reply_alias_key_t
{
    router_reply_alias_key_t ();

    zlink::pipe_t *pipe;
    int source_peer_socket_type;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    uint64_t wire_request_seq;

    bool operator== (const router_reply_alias_key_t &other_) const;
};

struct router_reply_alias_key_hash_t
{
    size_t operator() (const router_reply_alias_key_t &key_) const;
};

template <typename T> class reply_target_store_t
{
  public:
    struct node_t
    {
        node_t () : first (0), bucket_next (NULL), live_previous (NULL),
                    live_next (NULL), free_next (NULL), alias_next (NULL) {}
        uint64_t first;
        T second;
        node_t *bucket_next;
        node_t *live_previous;
        node_t *live_next;
        node_t *free_next;
        // Router targets also participate in the socket-owned physical-alias
        // index. Dealer targets leave this null.
        node_t *alias_next;
    };

    class iterator
    {
      public:
        iterator (node_t *node_ = NULL) : _node (node_) {}
        node_t &operator* () const { return *_node; }
        node_t *operator-> () const { return _node; }
        iterator &operator++ ()
        {
            _node = _node ? _node->live_next : NULL;
            return *this;
        }
        bool operator== (const iterator &other_) const
        {
            return _node == other_._node;
        }
        bool operator!= (const iterator &other_) const
        {
            return !(*this == other_);
        }

      private:
        friend class reply_target_store_t<T>;
        node_t *_node;
    };

    reply_target_store_t () : _free_head (NULL), _live_head (NULL),
                              _live_tail (NULL), _slabs (NULL), _size (0),
                              _capacity (inline_node_count)
    {
        memset (_buckets, 0, sizeof (_buckets));
        add_free_nodes (_inline_nodes, inline_node_count);
    }

    ~reply_target_store_t ()
    {
        while (_slabs) {
            slab_t *const next = _slabs->next;
            delete _slabs;
            _slabs = next;
        }
    }

    std::pair<iterator, bool> emplace (uint64_t key_, const T &value_)
    {
        if (key_ == 0 || find (key_) != end ()) {
            errno = EEXIST;
            return std::make_pair (end (), false);
        }
        if (!_free_head && !grow ())
            return std::make_pair (end (), false);
        node_t *const node = _free_head;
        _free_head = node->free_next;
        node->free_next = NULL;
        node->first = key_;
        node->second = value_;
        const size_t bucket = bucket_for (key_);
        node->bucket_next = _buckets[bucket];
        _buckets[bucket] = node;
        node->live_previous = _live_tail;
        node->live_next = NULL;
        if (_live_tail)
            _live_tail->live_next = node;
        else
            _live_head = node;
        _live_tail = node;
        ++_size;
        errno = 0;
        return std::make_pair (iterator (node), true);
    }

    iterator find (uint64_t key_)
    {
        node_t *node = key_ ? _buckets[bucket_for (key_)] : NULL;
        while (node && node->first != key_)
            node = node->bucket_next;
        return iterator (node);
    }
    iterator begin () { return iterator (_live_head); }
    iterator end () { return iterator (); }
    bool empty () const { return _size == 0; }
    size_t size () const { return _size; }

    iterator erase (iterator position_)
    {
        node_t *const node = position_._node;
        if (!node)
            return end ();
        node_t *const next = node->live_next;
        const size_t bucket = bucket_for (node->first);
        node_t **link = &_buckets[bucket];
        while (*link && *link != node)
            link = &(*link)->bucket_next;
        if (*link == node)
            *link = node->bucket_next;
        if (node->live_previous)
            node->live_previous->live_next = node->live_next;
        else
            _live_head = node->live_next;
        if (node->live_next)
            node->live_next->live_previous = node->live_previous;
        else
            _live_tail = node->live_previous;
        node->first = 0;
        node->second = T ();
        node->bucket_next = NULL;
        node->live_previous = NULL;
        node->live_next = NULL;
        node->alias_next = NULL;
        node->free_next = _free_head;
        _free_head = node;
        zlink_assert (_size != 0);
        --_size;
        return iterator (next);
    }
    void clear ()
    {
        while (!empty ())
            erase (begin ());
    }

  private:
    enum { inline_node_count = 64, slab_node_count = 64,
           bucket_count = 1024 };
    struct slab_t
    {
        slab_t () : next (NULL) {}
        node_t nodes[slab_node_count];
        slab_t *next;
    };
    static size_t bucket_for (uint64_t key_)
    {
        key_ ^= key_ >> 33;
        key_ *= UINT64_C (0xff51afd7ed558ccd);
        key_ ^= key_ >> 33;
        key_ *= UINT64_C (0xc4ceb9fe1a85ec53);
        key_ ^= key_ >> 33;
        return static_cast<size_t> (key_)
               & static_cast<size_t> (bucket_count - 1);
    }
    void add_free_nodes (node_t *nodes_, size_t count_)
    {
        for (size_t i = 0; i != count_; ++i) {
            nodes_[i].free_next = _free_head;
            _free_head = &nodes_[i];
        }
    }
    bool grow ()
    {
        if (_capacity >= max_reply_target_slots) {
            errno = EAGAIN;
            return false;
        }
        slab_t *const slab = new (std::nothrow) slab_t ();
        if (!slab) {
            errno = ENOMEM;
            return false;
        }
        slab->next = _slabs;
        _slabs = slab;
        add_free_nodes (slab->nodes, slab_node_count);
        _capacity += slab_node_count;
        return true;
    }

    node_t *_buckets[bucket_count];
    node_t _inline_nodes[inline_node_count];
    node_t *_free_head;
    node_t *_live_head;
    node_t *_live_tail;
    slab_t *_slabs;
    size_t _size;
    size_t _capacity;

    reply_target_store_t (const reply_target_store_t &) = delete;
    reply_target_store_t &operator= (const reply_target_store_t &) = delete;
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
    pending_request_store_t pending_requests;
    std::shared_ptr<zlink::request_timeout::task_t> pending_timeout_task;
    uint64_t pending_timeout_deadline_ns;
    uint64_t pending_timeout_generation;
    bool pending_timeout_dispatching;
    reply_target_store_t<dealer_reply_target_t> dealer_reply_targets;
    reply_target_store_t<router_reply_target_t> router_reply_targets;
    enum { router_reply_alias_bucket_count = 1024 };
    reply_target_store_t<router_reply_target_t>::node_t
      *router_reply_alias_buckets[router_reply_alias_bucket_count];
    size_t reply_target_slots;
    size_t reply_target_reservations;
    size_t reply_target_checkouts;
    uint64_t router_next_reply_token;
    std::atomic<uint64_t> public_router_reply_checkout_token;
    bool public_router_reply_active;
    std::thread::id public_router_reply_owner;
    uint64_t public_router_reply_token;
    router_reply_target_t public_router_reply_target;
    bool closing;
};

int recv_router_message_direct (const socket_handle_t &handle_,
                                const zlink_routing_id_t **source_node_rid_out_,
                                uint64_t *reply_token_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_,
                                zlink_msg_t *terminal_part_out_ = NULL,
                                bool *terminal_part_returned_out_ = NULL,
                                uint64_t *transport_pair_id_out_ = NULL,
                                uint64_t *transport_pair_generation_out_ = NULL);
int recv_dealer_message_direct (const socket_handle_t &handle_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                int flags_,
                                zlink_msg_t *terminal_part_out_ = NULL,
                                bool *terminal_part_returned_out_ = NULL,
                                bool public_part_receive_ = false,
                                bool *public_part_delivery_hold_out_ = NULL);
void forget_dealer_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_);
bool take_router_reply_target_locked (
  socket_request_reply_state_t *state_, uint64_t request_token_,
  const zlink_routing_id_t *peer_rid_,
  router_reply_target_t *target_out_);
void restore_router_reply_target (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t request_token_);
void revoke_router_reply_target (const socket_handle_t &handle_,
                                 const zlink_routing_id_t *peer_rid_,
                                 uint64_t request_seq_);
void forget_router_reply_targets_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  zlink::pipe_t *application_pipe_);
void revoke_router_reply_targets_for_rid (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const zlink_routing_id_t *peer_rid_);
void clear_router_reply_targets_locked (socket_request_reply_state_t *state_);
void abandon_public_router_reply_sequence (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t expected_token_);
void commit_public_router_reply_sequence (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t expected_token_);
int send_completion_staged_frames (zlink::socket_base_t *socket_,
                                   zlink::pipe_t *application_pipe_,
                                   const zlink_routing_id_t *peer_rid_,
                                   zlink_msg_t *staged_parts_,
                                   size_t staged_part_count_,
                                   zlink_msg_t *final_part_);
zlink::pipe_t *retain_reply_transport_pipe (
  zlink::socket_base_t *socket_, const router_reply_target_t &target_,
  const zlink_routing_id_t *peer_rid_);
zlink::pipe_t *retain_reply_completion_pipe (
  zlink::socket_base_t *socket_, zlink::pipe_t *application_pipe_,
  const zlink_routing_id_t *peer_rid_);
int send_completion_staged_frames_on_pipe (
  zlink::pipe_t *completion_pipe_, zlink_msg_t *staged_parts_,
  size_t staged_part_count_, zlink_msg_t *final_part_,
  bool preserve_initial_failure_,
  zlink::pipe_message_admission_t *first_admission_out_ = NULL);
std::shared_ptr<socket_request_reply_state_t>
find_or_create_request_reply_state (const socket_handle_t &handle_);
std::shared_ptr<socket_request_reply_state_t>
find_request_reply_state (const socket_handle_t &handle_);
void release_pending_request_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_);
int publish_pending_request_completion (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  pending_request_t *pending_, zlink_request_result_t result_,
  zlink_msg_t *parts_, size_t part_count_);
int add_socket_pending_request_locked (socket_request_reply_state_t *state_,
                                       pending_request_t pending_);
bool remove_socket_pending_request_locked (socket_request_reply_state_t *state_,
                                           const pending_request_identity_t &identity_,
                                           pending_request_t *pending_out_);
bool take_pending_reply_from_transport_locked (
  socket_request_reply_state_t *state_,
  uint64_t request_seq_,
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  zlink::pipe_t *source_pipe_, uint64_t source_connection_id_,
  pending_request_t *pending_out_);
bool take_next_socket_pending_request_for_logical_endpoint_locked (
  socket_request_reply_state_t *state_, const std::string &logical_endpoint_,
  pending_request_t *pending_out_);
bool take_next_socket_pending_request_for_pipe_locked (
  socket_request_reply_state_t *state_, const zlink::pipe_t *pipe_,
  pending_request_t *pending_out_);
bool take_next_socket_pending_request_for_transport_pair_locked (
  socket_request_reply_state_t *state_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_, pending_request_t *pending_out_);
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
void cancel_socket_pending_timeouts (
  const std::shared_ptr<socket_request_reply_state_t> &state_);
int ensure_socket_pull_pending_request (
  const socket_handle_t &handle_, uint32_t timeout_ms_,
  const zlink_routing_id_t *peer_rid_, void *user_context_,
  uint64_t *request_seq_out_,
  std::shared_ptr<socket_request_reply_state_t> *state_out_,
  pending_request_token_t *token_out_,
  zlink_completion_id_t *completion_id_out_);
bool has_pending_request_work (const std::shared_ptr<socket_request_reply_state_t> &state_);
void fail_pending_requests_for_logical_endpoint (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const std::string &logical_endpoint_);
void fail_pending_requests_for_pipe (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const zlink::pipe_t *pipe_);
void fail_pending_requests_for_transport_pair (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_);
int drain_close_request_reply_socket (const socket_handle_t &handle_);
void cleanup_request_reply_socket (const socket_handle_t &handle_);

#ifdef ZLINK_BUILD_TESTS
typedef void (*completion_pipe_budget_exhausted_test_hook_fn) (
  zlink::socket_base_t *socket_, zlink::pipe_t *pipe_, void *userdata_);

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
typedef void (*request_reply_write_after_prefix_hook_fn) (void *userdata_);

void test_set_request_reply_allocation_failpoint (
  request_reply_allocation_failpoint_t failpoint_);
void test_throw_request_reply_allocation_failpoint (
  request_reply_allocation_failpoint_t failpoint_);
void test_set_request_reply_write_failure_after_prefix (bool enabled_);
bool test_take_request_reply_write_failure_after_prefix ();
void test_set_request_reply_write_after_prefix_hook (
  request_reply_write_after_prefix_hook_fn hook_, void *userdata_);
void test_invoke_request_reply_write_after_prefix_hook ();
void test_set_request_reply_timeout_after_remove_hook (
  request_reply_timeout_after_remove_hook_fn hook_, void *userdata_);
void test_set_completion_pipe_budget_exhausted_hook (
  completion_pipe_budget_exhausted_test_hook_fn hook_, void *userdata_);
#endif
}
}

#endif
