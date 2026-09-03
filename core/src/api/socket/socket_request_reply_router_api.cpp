/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

namespace
{
void export_router_recv_part_metadata_view (zlink::socket_base_t *socket_,
                                            const zlink_routing_id_t *source_node_rid_,
                                            uint64_t reply_token_,
                                            const zlink_routing_id_t **source_node_rid_out_,
                                            uint64_t *reply_token_out_,
                                            uint64_t transport_pair_id_,
                                            uint64_t transport_pair_generation_,
                                            uint64_t *transport_pair_id_out_ = NULL,
                                            uint64_t *transport_pair_generation_out_ = NULL)
{
    if (socket_)
        socket_->store_last_recv_source_rid (source_node_rid_);
    if (source_node_rid_out_)
        *source_node_rid_out_ = socket_ ? socket_->last_recv_source_rid_view () : NULL;
    if (reply_token_out_)
        *reply_token_out_ = reply_token_;
    if (transport_pair_id_out_)
        *transport_pair_id_out_ = transport_pair_id_;
    if (transport_pair_generation_out_)
        *transport_pair_generation_out_ = transport_pair_generation_;
}

int take_staged_router_recv_part (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink::socket_base_t *socket_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *reply_token_out_,
  uint64_t *transport_pair_id_out_ = NULL,
  uint64_t *transport_pair_generation_out_ = NULL)
{
    const zlink_routing_id_t *source_node_rid = NULL;
    uint64_t reply_token = 0;
    uint64_t transport_pair_id = 0;
    uint64_t transport_pair_generation = 0;
    const int rc = zlink::part_helper_internal::take_recv_part (
      state_, part_out_, has_more_out_, &source_node_rid, &reply_token,
      &transport_pair_id, &transport_pair_generation);
    if (rc == 0)
        export_router_recv_part_metadata_view (
          socket_, source_node_rid, reply_token, source_node_rid_out_,
          reply_token_out_, transport_pair_id, transport_pair_generation,
          transport_pair_id_out_, transport_pair_generation_out_);
    return rc;
}

void revoke_router_receive_publication (
  const socket_handle_t &handle_,
  const zlink_routing_id_t *source_node_rid_,
  uint64_t reply_token_)
{
    const int saved_errno = errno;
    reqrep::revoke_router_reply_target (
      handle_, source_node_rid_, reply_token_);
    errno = saved_errno;
}

void revoke_staged_router_receive_publication (
  const socket_handle_t &handle_,
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_)
{
    if (!state_)
        return;
    zlink_routing_id_t source_node_rid;
    uint64_t reply_token = 0;
    bool source_rid_present = false;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        source_node_rid = state_->recv.source_node_rid;
        reply_token = state_->recv.request_seq;
        source_rid_present = !state_->recv.return_source_rid_as_null;
    }
    revoke_router_receive_publication (
      handle_, source_rid_present ? &source_node_rid : NULL, reply_token);
}

int stage_router_recv_sequence (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink::socket_base_t *source_socket_,
  const zlink_routing_id_t *source_node_rid_, uint64_t reply_token_,
  zlink_msg_t *parts_, size_t part_count_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_)
{
    int rc = -1;
    try {
#ifdef ZLINK_BUILD_TESTS
        reqrep::test_throw_request_reply_allocation_failpoint (
          reqrep::request_reply_allocation_receive_part_stage);
#endif
        rc = zlink::part_helper_internal::stage_recv_sequence (
          state_, zlink::part_helper_internal::recv_family_router,
          source_socket_, source_node_rid_, reply_token_, parts_, part_count_,
          std::this_thread::get_id (), transport_pair_id_,
          transport_pair_generation_);
    } catch (...) {
        errno = ENOMEM;
    }
    zlink_multipart_close (parts_, part_count_);
    return rc;
}

}

static zlink_recv_result_t router_recv_part_impl (
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *reply_token_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_,
  uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_)
{
    if (!router_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);
    handle.socket->clear_last_recv_source_rid ();
    if (!source_node_rid_out_ || !reply_token_out_ || !part_out_
        || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_ROUTER) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      handle.socket->part_helper_state ();

    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (helper_state);
    auto recv_router_parts_once = [&] (const zlink_routing_id_t **source_node_rid_out,
                                       uint64_t *reply_token_out, zlink_msg_t **parts_out,
                                       size_t *part_count_out,
                                       zlink_msg_t *terminal_part_out,
                                       bool *terminal_part_returned_out,
                                       uint64_t *transport_pair_id_out,
                                       uint64_t *transport_pair_generation_out) -> zlink_recv_result_t {
        return reqrep::recv_router_message_direct (
                 handle, source_node_rid_out, reply_token_out, parts_out, part_count_out,
                 static_cast<int> (flags_), terminal_part_out,
                 terminal_part_returned_out, transport_pair_id_out,
                 transport_pair_generation_out)
                 == 0
               ? ZLINK_RECV_OK
               : zlink::recv_result_internal::from_errno (errno);
    };

    if (!recv_sequence_active) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t reply_token = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        bool terminal_part_returned = false;
        uint64_t transport_pair_id = 0;
        uint64_t transport_pair_generation = 0;
        const zlink_recv_result_t recv_rc = recv_router_parts_once (
          &source_node_rid, &reply_token, &parts, &part_count, part_out_,
          &terminal_part_returned, &transport_pair_id,
          &transport_pair_generation);
        if (recv_rc != ZLINK_RECV_OK)
            return zlink::recv_result_internal::from_errno (errno);

        if (terminal_part_returned) {
            export_router_recv_part_metadata_view (
              handle.socket, source_node_rid, reply_token,
              source_node_rid_out_, reply_token_out_, transport_pair_id,
              transport_pair_generation, transport_pair_id_out_,
              transport_pair_generation_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!parts || part_count == 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   reply_token);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            export_router_recv_part_metadata_view (
              handle.socket, source_node_rid, reply_token,
              source_node_rid_out_, reply_token_out_, transport_pair_id,
              transport_pair_generation, transport_pair_id_out_,
              transport_pair_generation_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!helper_state) {
            helper_state =
              zlink::part_helper_internal::find_or_create_socket_state (handle.socket);
            if (!helper_state) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   reply_token);
                return zlink::recv_result_internal::from_errno (errno);
            }
        }

        const int stage_rc = stage_router_recv_sequence (
          helper_state, handle.socket, source_node_rid, reply_token, parts,
          part_count, transport_pair_id, transport_pair_generation);
        if (stage_rc != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (take_staged_router_recv_part (
              helper_state, handle.socket, part_out_, has_more_out_,
              source_node_rid_out_, reply_token_out_, transport_pair_id_out_,
              transport_pair_generation_out_)
            != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
        return ZLINK_RECV_OK;
    }

    zlink::socket_base_t *source_socket = handle.socket;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_recv_step (
          zlink::part_helper_internal::recv_family_router, source_socket, helper_state,
          &first_part, &source_socket)
        != 0) {
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (first_part) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t reply_token = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        uint64_t transport_pair_id = 0;
        uint64_t transport_pair_generation = 0;
        const zlink_recv_result_t recv_rc = recv_router_parts_once (
          &source_node_rid, &reply_token, &parts, &part_count, NULL, NULL,
          &transport_pair_id, &transport_pair_generation);
        if (recv_rc != ZLINK_RECV_OK) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!parts || part_count == 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   reply_token);
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            export_router_recv_part_metadata_view (
              handle.socket, source_node_rid, reply_token,
              source_node_rid_out_, reply_token_out_, transport_pair_id,
              transport_pair_generation, transport_pair_id_out_,
              transport_pair_generation_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }

        const int stage_rc = stage_router_recv_sequence (
          helper_state, source_socket, source_node_rid, reply_token, parts,
          part_count, transport_pair_id, transport_pair_generation);
        if (stage_rc != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (take_staged_router_recv_part (
              helper_state, handle.socket, part_out_, has_more_out_,
              source_node_rid_out_, reply_token_out_, transport_pair_id_out_,
              transport_pair_generation_out_)
            != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    } else {
        if (take_staged_router_recv_part (
              helper_state, handle.socket, part_out_, has_more_out_,
              source_node_rid_out_, reply_token_out_, transport_pair_id_out_,
              transport_pair_generation_out_)
            != 0) {
            revoke_staged_router_receive_publication (handle, helper_state);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_router_recv_part (
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *reply_token_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_)
{
    return router_recv_part_impl (
      router_, source_node_rid_out_, reply_token_out_, part_out_,
      has_more_out_, flags_, NULL, NULL);
}
