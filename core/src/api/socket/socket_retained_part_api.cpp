/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string>
#include <vector>
#include <new>

#include "api/message/recv_result_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "core/recv_internal.hpp"
#include "sockets/common/socket_base.hpp"

struct zlink_hwm_budget_lease_t
{
    std::shared_ptr<zlink::retained_credit_origin_t> origin;
};

namespace
{
namespace reqrep = zlink::socket_reqrep_internal;

int retained_token_to_public_lease (
  zlink::socket_base_t *socket_, zlink::retained_credit_token_t *token_,
  zlink_hwm_budget_lease_t **lease_out_)
{
    if (!socket_ || !token_ || !lease_out_) {
        errno = EFAULT;
        return -1;
    }
    *lease_out_ = NULL;
    if (token_->empty ())
        return 0;
    if (socket_->ensure_async_command_processing () != 0)
        return -1;

    std::shared_ptr<zlink::retained_credit_origin_t> origin;
    if (token_->transfer_to_application (&origin) != 0)
        return -1;

    zlink_hwm_budget_lease_t *lease =
      new (std::nothrow) zlink_hwm_budget_lease_t ();
    if (!lease) {
        zlink::release_retained_credit_origin (&origin);
        errno = ENOMEM;
        return -1;
    }
    lease->origin.swap (origin);
    *lease_out_ = lease;
    return 0;
}

zlink_routing_id_t &retained_part_source_rid_tls ()
{
    static thread_local zlink_routing_id_t rid;
    return rid;
}

zlink_recv_result_t fail_recv ()
{
    return zlink::recv_result_internal::from_errno (errno);
}

zlink_recv_result_t finish_part (
  zlink::socket_base_t *socket_, zlink_msg_t *part_out_,
  zlink::retained_credit_token_t *credit_,
  zlink_hwm_budget_lease_t **lease_out_)
{
    if (retained_token_to_public_lease (socket_, credit_, lease_out_) == 0)
        return ZLINK_RECV_OK;

    const int saved_errno = errno;
    zlink_msg_close (part_out_);
    zlink_msg_init (part_out_);
    errno = saved_errno;
    return fail_recv ();
}

int move_single_part (
  zlink_msg_t *parts_, size_t part_count_,
  std::vector<zlink::retained_credit_token_t> *credits_,
  zlink_msg_t *part_out_, zlink::retained_credit_token_t *credit_out_)
{
    if (!parts_ || part_count_ != 1 || !credits_ || credits_->size () != 1) {
        errno = EPROTO;
        return -1;
    }
    if (zlink_msg_move (part_out_, &parts_[0]) != 0) {
        errno = EFAULT;
        return -1;
    }
    *credit_out_ = std::move ((*credits_)[0]);
    return 0;
}

int stage_and_take (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink::part_helper_internal::recv_family_t family_,
  zlink::socket_base_t *source_socket_,
  const zlink_routing_id_t *source_rid_, uint64_t request_seq_,
  zlink_msg_t *parts_, size_t part_count_,
  std::vector<zlink::retained_credit_token_t> *credits_,
  zlink_msg_t *part_out_, zlink::retained_credit_token_t *credit_out_,
  zlink_part_flag_t *has_more_out_)
{
    if (zlink::part_helper_internal::stage_recv_sequence (
          state_, family_, source_socket_, source_rid_, request_seq_, parts_,
          part_count_, std::this_thread::get_id (), credits_)
        != 0)
        return -1;
    return zlink::part_helper_internal::take_recv_part (
      state_, part_out_, has_more_out_, credit_out_);
}

} // namespace

void zlink_hwm_budget_lease_release (zlink_hwm_budget_lease_t **lease_p_)
{
    if (!lease_p_ || !*lease_p_)
        return;
    zlink_hwm_budget_lease_t *lease = *lease_p_;
    *lease_p_ = NULL;
    zlink::release_retained_credit_origin (&lease->origin);
    delete lease;
}

int zlink_recv_with_hwm_budget_lease (
  void *socket_, zlink_msg_t *message_,
  zlink_hwm_budget_lease_t **lease_out_, int flags_)
{
    if (!socket_ || !message_ || !lease_out_) {
        errno = EFAULT;
        return -1;
    }
    *lease_out_ = NULL;
    if (validate_recv_flags (static_cast<zlink_recv_flags_t> (flags_)) != 0)
        return -1;

    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;

    zlink::retained_credit_token_t token;
    const int rc = zlink::recv_msg_socket_retained (
      handle.socket, socket_type (handle), message_, &token, flags_);
    if (rc < 0)
        return -1;
    if (retained_token_to_public_lease (handle.socket, &token, lease_out_)
        != 0) {
        const int saved_errno = errno;
        zlink_msg_close (message_);
        zlink_msg_init (message_);
        errno = saved_errno;
        return -1;
    }
    return rc;
}

zlink_recv_result_t zlink_recv_part_with_hwm_budget_lease (
  void *s_, const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_, zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_)
{
    if (!s_ || !part_out_ || !lease_out_ || !has_more_out_) {
        errno = EFAULT;
        return fail_recv ();
    }
    *lease_out_ = NULL;
    if (validate_recv_flags (flags_) != 0)
        return fail_recv ();

    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return fail_recv ();
    const int type = socket_type (handle);
    if (type == ZLINK_CORE_SOCKET_PUB || type == ZLINK_CORE_SOCKET_XPUB
        || type == ZLINK_CORE_SOCKET_SUB || type == ZLINK_CORE_SOCKET_XSUB
        || type == ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return fail_recv ();
    }

    const bool expose_source_rid = type == ZLINK_CORE_SOCKET_STREAM;
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper =
      zlink::part_helper_internal::find_handle_state (s_);
    zlink::retained_credit_token_t credit;

    if (zlink::part_helper_internal::recv_sequence_active (helper)) {
        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              s_, zlink::part_helper_internal::recv_family_basic,
              handle.socket, &helper, &first_part, &source_socket)
            != 0
            || first_part
            || zlink::part_helper_internal::take_recv_part (
                 helper, part_out_, has_more_out_, &credit)
                 != 0) {
            zlink::part_helper_internal::abort_recv_step (helper);
            return fail_recv ();
        }
        if (source_rid_out_)
            zlink::part_helper_internal::export_recv_metadata (
              helper, source_rid_out_, NULL);
        zlink::part_helper_internal::complete_recv_step (helper,
                                                         *has_more_out_);
        return finish_part (handle.socket, part_out_, &credit, lease_out_);
    }

    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    std::vector<zlink::retained_credit_token_t> credits;
    int recv_rc = 0;
    if (type == ZLINK_CORE_SOCKET_DEALER) {
        std::shared_ptr<reqrep::socket_request_reply_state_t> request_state =
          reqrep::find_or_create_request_reply_state (handle);
        uint8_t ignored_type = 0;
        uint64_t ignored_seq = 0;
        recv_rc = request_state
                    ? reqrep::recv_dealer_message_direct (
                        handle, request_state, &ignored_type, &ignored_seq,
                        &parts, &part_count, static_cast<int> (flags_),
                        &credits)
                    : -1;
    } else {
        recv_rc = zlink::socket_recv_internal_retained (
          s_, expose_source_rid ? &source_rid : NULL, &parts, &part_count,
          static_cast<zlink_send_flags_t> (flags_), &credits);
    }
    if (recv_rc != 0)
        return fail_recv ();

    if (part_count == 1) {
        const int move_rc = move_single_part (
          parts, part_count, &credits, part_out_, &credit);
        zlink_multipart_close (parts, part_count);
        if (move_rc != 0)
            return fail_recv ();
        if (source_rid_out_) {
            zlink_routing_id_t &view = retained_part_source_rid_tls ();
            view = source_rid;
            *source_rid_out_ = expose_source_rid && view.size > 0
                                 ? &view
                                 : NULL;
        }
        *has_more_out_ = ZLINK_PART_FINAL;
        return finish_part (handle.socket, part_out_, &credit, lease_out_);
    }

    helper = zlink::part_helper_internal::find_or_create_handle_state (s_);
    if (!helper) {
        zlink_multipart_close (parts, part_count);
        return fail_recv ();
    }
    const int stage_rc = stage_and_take (
      helper, zlink::part_helper_internal::recv_family_basic, handle.socket,
      expose_source_rid ? &source_rid : NULL, 0, parts, part_count, &credits,
      part_out_, &credit, has_more_out_);
    zlink_multipart_close (parts, part_count);
    if (stage_rc != 0) {
        zlink::part_helper_internal::abort_recv_step (helper);
        return fail_recv ();
    }
    if (source_rid_out_)
        zlink::part_helper_internal::export_recv_metadata (
          helper, source_rid_out_, NULL);
    zlink::part_helper_internal::complete_recv_step (helper,
                                                     *has_more_out_);
    return finish_part (handle.socket, part_out_, &credit, lease_out_);
}

zlink_recv_result_t zlink_dealer_recv_part_with_hwm_budget_lease (
  void *dealer_, uint8_t *message_type_out_, uint64_t *request_seq_out_,
  zlink_msg_t *part_out_, zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_)
{
    if (!dealer_ || !message_type_out_ || !request_seq_out_ || !part_out_
        || !lease_out_ || !has_more_out_) {
        errno = EFAULT;
        return fail_recv ();
    }
    *lease_out_ = NULL;
    if (validate_recv_flags (flags_) != 0)
        return fail_recv ();
    socket_handle_t handle = as_socket_handle (dealer_);
    if (!handle.socket || socket_type (handle) != ZLINK_CORE_SOCKET_DEALER) {
        errno = EINVAL;
        return fail_recv ();
    }
    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state =
      reqrep::find_or_create_request_reply_state (handle);
    if (!request_state)
        return fail_recv ();

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper =
      zlink::part_helper_internal::find_handle_state (dealer_);
    zlink::retained_credit_token_t credit;
    if (zlink::part_helper_internal::recv_sequence_active (helper)) {
        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              dealer_, zlink::part_helper_internal::recv_family_dealer,
              handle.socket, &helper, &first_part, &source_socket)
            != 0
            || first_part
            || zlink::part_helper_internal::take_recv_part (
                 helper, part_out_, has_more_out_, &credit)
                 != 0) {
            zlink::part_helper_internal::abort_recv_step (helper);
            return fail_recv ();
        }
        {
            std::lock_guard<std::mutex> lock (helper->mutex);
            *message_type_out_ = helper->recv.message_type;
            *request_seq_out_ = helper->recv.request_seq;
        }
        zlink::part_helper_internal::complete_recv_step (helper,
                                                         *has_more_out_);
        return finish_part (handle.socket, part_out_, &credit, lease_out_);
    }

    uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    std::vector<zlink::retained_credit_token_t> credits;
    if (reqrep::recv_dealer_message_direct (
          handle, request_state, &message_type, &request_seq, &parts,
          &part_count, static_cast<int> (flags_), &credits)
        != 0)
        return fail_recv ();

    if (part_count == 1) {
        const int move_rc = move_single_part (
          parts, part_count, &credits, part_out_, &credit);
        zlink_multipart_close (parts, part_count);
        if (move_rc != 0)
            return fail_recv ();
        *message_type_out_ = message_type;
        *request_seq_out_ = request_seq;
        *has_more_out_ = ZLINK_PART_FINAL;
        return finish_part (handle.socket, part_out_, &credit, lease_out_);
    }

    helper = zlink::part_helper_internal::find_or_create_handle_state (dealer_);
    if (!helper) {
        zlink_multipart_close (parts, part_count);
        return fail_recv ();
    }
    const int stage_rc = stage_and_take (
      helper, zlink::part_helper_internal::recv_family_dealer, handle.socket,
      NULL, request_seq, parts, part_count, &credits, part_out_, &credit,
      has_more_out_);
    zlink_multipart_close (parts, part_count);
    if (stage_rc != 0) {
        zlink::part_helper_internal::abort_recv_step (helper);
        return fail_recv ();
    }
    {
        std::lock_guard<std::mutex> lock (helper->mutex);
        helper->recv.message_type = message_type;
        *message_type_out_ = message_type;
        *request_seq_out_ = request_seq;
    }
    zlink::part_helper_internal::complete_recv_step (helper,
                                                     *has_more_out_);
    return finish_part (handle.socket, part_out_, &credit, lease_out_);
}

zlink_recv_result_t zlink_router_recv_part_v2_with_hwm_budget_lease (
  void *router_, const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_, uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_, zlink_msg_t *part_out_,
  zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_)
{
    if (!router_ || !source_node_rid_out_ || !request_seq_out_
        || !transport_pair_id_out_ || !transport_pair_generation_out_
        || !part_out_ || !lease_out_ || !has_more_out_) {
        errno = EFAULT;
        return fail_recv ();
    }
    *lease_out_ = NULL;
    *transport_pair_id_out_ = 0;
    *transport_pair_generation_out_ = 0;
    if (validate_recv_flags (flags_) != 0)
        return fail_recv ();
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket || socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER) {
        errno = EINVAL;
        return fail_recv ();
    }
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper =
      zlink::part_helper_internal::find_handle_state (router_);
    zlink::retained_credit_token_t credit;
    if (zlink::part_helper_internal::recv_sequence_active (helper)) {
        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              router_, zlink::part_helper_internal::recv_family_router,
              handle.socket, &helper, &first_part, &source_socket)
            != 0
            || first_part
            || zlink::part_helper_internal::take_recv_part (
                 helper, part_out_, has_more_out_, &credit)
                 != 0) {
            zlink::part_helper_internal::abort_recv_step (helper);
            return fail_recv ();
        }
        zlink::part_helper_internal::export_recv_metadata (
          helper, source_node_rid_out_, request_seq_out_);
        zlink::part_helper_internal::export_recv_transport_pair (
          helper, transport_pair_id_out_, transport_pair_generation_out_);
        zlink::part_helper_internal::complete_recv_step (helper,
                                                         *has_more_out_);
        return finish_part (handle.socket, part_out_, &credit, lease_out_);
    }

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    std::vector<zlink::retained_credit_token_t> credits;
    if (reqrep::recv_router_message_direct (
          handle, &source_rid, &request_seq, &parts, &part_count,
          static_cast<int> (flags_), &credits)
        != 0)
        return fail_recv ();
    const reqrep::router_recv_metadata_tls_t metadata =
      reqrep::router_recv_metadata_tls ();

    if (part_count == 1) {
        const int move_rc = move_single_part (
          parts, part_count, &credits, part_out_, &credit);
        zlink_multipart_close (parts, part_count);
        if (move_rc != 0)
            return fail_recv ();
        zlink_routing_id_t &view = retained_part_source_rid_tls ();
        zlink::part_helper_internal::copy_routing_id (source_rid, &view);
        *source_node_rid_out_ = source_rid ? &view : NULL;
        *request_seq_out_ = request_seq;
        *transport_pair_id_out_ = metadata.transport_pair_id;
        *transport_pair_generation_out_ = metadata.transport_pair_generation;
        *has_more_out_ = ZLINK_PART_FINAL;
        return finish_part (handle.socket, part_out_, &credit, lease_out_);
    }

    helper = zlink::part_helper_internal::find_or_create_handle_state (router_);
    if (!helper) {
        zlink_multipart_close (parts, part_count);
        return fail_recv ();
    }
    const int stage_rc = stage_and_take (
      helper, zlink::part_helper_internal::recv_family_router, handle.socket,
      source_rid, request_seq, parts, part_count, &credits, part_out_,
      &credit, has_more_out_);
    zlink_multipart_close (parts, part_count);
    if (stage_rc != 0) {
        zlink::part_helper_internal::abort_recv_step (helper);
        return fail_recv ();
    }
    zlink::part_helper_internal::set_recv_transport_pair (
      &helper->recv, metadata.transport_pair_id,
      metadata.transport_pair_generation);
    zlink::part_helper_internal::export_recv_metadata (
      helper, source_node_rid_out_, request_seq_out_);
    zlink::part_helper_internal::export_recv_transport_pair (
      helper, transport_pair_id_out_, transport_pair_generation_out_);
    zlink::part_helper_internal::complete_recv_step (helper,
                                                     *has_more_out_);
    return finish_part (handle.socket, part_out_, &credit, lease_out_);
}

zlink_recv_result_t zlink_subscribe_part_with_hwm_budget_lease (
  void *sub_, const zlink_routing_id_t **source_rid_out_,
  char *topic_id_buf_, size_t topic_id_capacity_,
  size_t *topic_id_len_out_, zlink_msg_t *part_out_,
  zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_)
{
    if (!sub_ || !topic_id_len_out_ || !part_out_ || !lease_out_
        || !has_more_out_) {
        errno = EFAULT;
        return fail_recv ();
    }
    *lease_out_ = NULL;
    if (topic_id_capacity_ > 0 && !topic_id_buf_) {
        errno = EFAULT;
        return fail_recv ();
    }
    if (validate_recv_flags (flags_) != 0)
        return fail_recv ();
    socket_handle_t handle = as_socket_handle (sub_);
    if (!handle.socket
        || (socket_type (handle) != ZLINK_CORE_SOCKET_SUB
            && socket_type (handle) != ZLINK_CORE_SOCKET_XSUB)) {
        errno = handle.socket ? ENOTSUP : EFAULT;
        return fail_recv ();
    }
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper =
      zlink::part_helper_internal::find_or_create_handle_state (sub_);
    if (!helper)
        return fail_recv ();
    if (zlink::part_helper_internal::recv_sequence_active (helper)) {
        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              sub_, zlink::part_helper_internal::recv_family_subscribe,
              handle.socket, &helper, &first_part, &source_socket)
            != 0 || first_part) {
            zlink::part_helper_internal::abort_recv_step (helper);
            return fail_recv ();
        }
    } else {
        zlink::retained_credit_token_t topic_credit;
        zlink::msg_t topic;
        const int init_rc = topic.init ();
        errno_assert (init_rc == 0);
        if (handle.socket->recv_retained (&topic, &topic_credit, flags_) != 0) {
            topic.close ();
            return fail_recv ();
        }
        topic_credit.reset ();
        const bool topic_more = (topic.flags () & zlink::msg_t::more) != 0;
        std::string topic_id (static_cast<const char *> (topic.data ()),
                              topic.size ());
        topic.close ();
        if (!topic_more) {
            errno = EPROTO;
            return fail_recv ();
        }

        std::vector<zlink_msg_t> parts;
        std::vector<zlink::retained_credit_token_t> credits;
        while (true) {
            parts.push_back (zlink_msg_t ());
            zlink_msg_init (&parts.back ());
            credits.push_back (zlink::retained_credit_token_t ());
            if (handle.socket->recv_retained (
                  reinterpret_cast<zlink::msg_t *> (&parts.back ()),
                  &credits.back (), ZLINK_DONTWAIT)
                != 0) {
                for (size_t i = 0; i < parts.size (); ++i)
                    zlink_msg_close (&parts[i]);
                return fail_recv ();
            }
            if (!zlink::msg_frame_has_more (parts.back ()))
                break;
        }

        if (zlink::part_helper_internal::stage_recv_sequence (
              helper, zlink::part_helper_internal::recv_family_subscribe,
              handle.socket, NULL, 0, &parts[0], parts.size (),
              std::this_thread::get_id (), &credits)
            != 0) {
            const int saved_errno = errno;
            for (size_t i = 0; i < parts.size (); ++i)
                zlink_msg_close (&parts[i]);
            zlink::part_helper_internal::abort_recv_step (helper);
            errno = saved_errno;
            return fail_recv ();
        }
        for (size_t i = 0; i < parts.size (); ++i)
            zlink_msg_close (&parts[i]);
        {
            std::lock_guard<std::mutex> lock (helper->mutex);
            helper->recv.topic_id.swap (topic_id);
        }
    }

    int copy_errno = 0;
    {
        std::lock_guard<std::mutex> lock (helper->mutex);
        *topic_id_len_out_ = helper->recv.topic_id.size ();
        if (topic_id_capacity_ == 0
            || topic_id_capacity_ < helper->recv.topic_id.size ())
            copy_errno = ENOBUFS;
        else if (!helper->recv.topic_id.empty ())
            memcpy (topic_id_buf_, helper->recv.topic_id.data (),
                    helper->recv.topic_id.size ());
    }
    if (copy_errno != 0) {
        errno = copy_errno;
        return fail_recv ();
    }

    zlink::retained_credit_token_t credit;
    if (zlink::part_helper_internal::take_recv_part (
          helper, part_out_, has_more_out_, &credit)
        != 0) {
        zlink::part_helper_internal::abort_recv_step (helper);
        return fail_recv ();
    }
    if (source_rid_out_)
        *source_rid_out_ = NULL;
    zlink::part_helper_internal::complete_recv_step (helper,
                                                     *has_more_out_);
    return finish_part (handle.socket, part_out_, &credit, lease_out_);
}
