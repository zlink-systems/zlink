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
struct router_recv_part_metadata_tls_t
{
    zlink_routing_id_t source_node_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
};

router_recv_part_metadata_tls_t &router_recv_part_metadata_tls ()
{
    static thread_local router_recv_part_metadata_tls_t metadata;
    return metadata;
}

void export_router_recv_part_metadata_view (const zlink_routing_id_t *source_node_rid_,
                                            uint64_t request_seq_,
                                            const zlink_routing_id_t **source_node_rid_out_,
                                            uint64_t *request_seq_out_,
                                            uint64_t *transport_pair_id_out_ = NULL,
                                            uint64_t *transport_pair_generation_out_ = NULL)
{
    router_recv_part_metadata_tls_t &metadata = router_recv_part_metadata_tls ();
    if (source_node_rid_ != &metadata.source_node_rid)
        zlink::part_helper_internal::copy_routing_id (
          source_node_rid_, &metadata.source_node_rid);
    const reqrep::router_recv_metadata_tls_t &source_metadata =
      reqrep::router_recv_metadata_tls ();
    metadata.transport_pair_id = source_metadata.transport_pair_id;
    metadata.transport_pair_generation = source_metadata.transport_pair_generation;

    if (source_node_rid_out_) {
        *source_node_rid_out_ = source_node_rid_ ? &metadata.source_node_rid : NULL;
    }
    if (request_seq_out_)
        *request_seq_out_ = request_seq_;
    if (transport_pair_id_out_)
        *transport_pair_id_out_ = metadata.transport_pair_id;
    if (transport_pair_generation_out_)
        *transport_pair_generation_out_ = metadata.transport_pair_generation;
}

int take_staged_router_recv_part (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_)
{
    router_recv_part_metadata_tls_t &metadata = router_recv_part_metadata_tls ();
    return zlink::part_helper_internal::take_recv_part (
      state_, part_out_, has_more_out_, source_node_rid_out_, request_seq_out_,
      &metadata.transport_pair_id,
      &metadata.transport_pair_generation);
}

void revoke_router_receive_publication (
  const socket_handle_t &handle_,
  const zlink_routing_id_t *source_node_rid_,
  uint64_t request_seq_)
{
    const int saved_errno = errno;
    reqrep::revoke_router_reply_target (handle_, source_node_rid_, request_seq_);
    errno = saved_errno;
}

void revoke_staged_router_receive_publication (
  const socket_handle_t &handle_,
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_)
{
    if (!state_)
        return;
    zlink_routing_id_t source_node_rid;
    uint64_t request_seq = 0;
    bool source_rid_present = false;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        source_node_rid = state_->recv.source_node_rid;
        request_seq = state_->recv.request_seq;
        source_rid_present = !state_->recv.return_source_rid_as_null;
    }
    revoke_router_receive_publication (
      handle_, source_rid_present ? &source_node_rid : NULL, request_seq);
}

void revoke_dealer_receive_publication (const socket_handle_t &handle_,
                                        uint64_t request_seq_)
{
    const int saved_errno = errno;
    reqrep::revoke_dealer_reply_target (handle_, request_seq_);
    errno = saved_errno;
}

void revoke_staged_dealer_receive_publication (
  const socket_handle_t &handle_,
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_)
{
    if (!state_)
        return;
    uint64_t request_seq = 0;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        request_seq = state_->recv.request_seq;
    }
    revoke_dealer_receive_publication (handle_, request_seq);
}

void maybe_fail_receive_part_stage ()
{
#ifdef ZLINK_BUILD_TESTS
    reqrep::test_throw_request_reply_allocation_failpoint (
      reqrep::request_reply_allocation_receive_part_stage);
#endif
}

}

zlink_recv_result_t zlink_router_recv_part (void *router_,
                                            const zlink_routing_id_t **source_node_rid_out_,
                                            uint64_t *request_seq_out_,
                                            zlink_msg_t *part_out_,
                                            zlink_part_flag_t *has_more_out_,
                                            zlink_recv_flags_t flags_)
{
    if (!router_ || !source_node_rid_out_ || !request_seq_out_
        || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_ROUTER) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    // ROUTER is a native socket handle here, so its optional multipart state
    // is owned by the socket. Do not route the steady-state raw role through
    // the generic foreign-handle registry.
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      handle.socket->has_part_helper_state ()
        ? handle.socket->part_helper_state ()
        : std::shared_ptr<zlink::part_helper_internal::handle_state_t> ();

    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (helper_state);
    auto recv_router_parts_once = [&] (const zlink_routing_id_t **source_node_rid_out,
                                       uint64_t *request_seq_out, zlink_msg_t **parts_out,
                                       size_t *part_count_out,
                                       zlink_msg_t *terminal_part_out,
                                       bool *terminal_part_returned_out,
                                       zlink_routing_id_t *terminal_source_storage) -> zlink_recv_result_t {
        return reqrep::recv_router_message_direct (
                 handle, source_node_rid_out, request_seq_out, parts_out, part_count_out,
                 static_cast<int> (flags_), terminal_part_out,
                 terminal_part_returned_out, terminal_source_storage)
                 == 0
               ? ZLINK_RECV_OK
               : zlink::recv_result_internal::from_errno (errno);
    };

    if (!recv_sequence_active) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        bool terminal_part_returned = false;
        router_recv_part_metadata_tls_t &part_metadata =
          router_recv_part_metadata_tls ();
        const zlink_recv_result_t recv_rc = recv_router_parts_once (
          &source_node_rid, &request_seq, &parts, &part_count, part_out_,
          &terminal_part_returned, &part_metadata.source_node_rid);
        if (recv_rc != ZLINK_RECV_OK)
            return zlink::recv_result_internal::from_errno (errno);

        if (terminal_part_returned) {
            // The direct terminal receive bypasses multipart staging, but the
            // v2 receive surface still promises the exact transport-pair
            // metadata captured by recv_router_message_direct().
            export_router_recv_part_metadata_view (
              source_node_rid, request_seq, source_node_rid_out_, request_seq_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!parts || part_count == 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   request_seq);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            export_router_recv_part_metadata_view (source_node_rid, request_seq,
                                                   source_node_rid_out_, request_seq_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!helper_state) {
            helper_state =
              zlink::part_helper_internal::find_or_create_socket_state (handle.socket);
            if (!helper_state) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   request_seq);
                return zlink::recv_result_internal::from_errno (errno);
            }
        }

        int stage_rc = -1;
        try {
            maybe_fail_receive_part_stage ();
            stage_rc = zlink::part_helper_internal::stage_recv_sequence (
              helper_state, zlink::part_helper_internal::recv_family_router,
              handle.socket, source_node_rid, request_seq, parts, part_count,
              std::this_thread::get_id ());
        } catch (...) {
            errno = ENOMEM;
        }
        const reqrep::router_recv_metadata_tls_t &recv_metadata =
          reqrep::router_recv_metadata_tls ();
        zlink::part_helper_internal::set_recv_transport_pair (
          helper_state ? &helper_state->recv : NULL,
          recv_metadata.transport_pair_id, recv_metadata.transport_pair_generation);
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (helper_state->recv.buffered_parts.empty ()) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (take_staged_router_recv_part (
              helper_state, part_out_, has_more_out_, source_node_rid_out_,
              request_seq_out_)
            != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
        return ZLINK_RECV_OK;
    }

    zlink::socket_base_t *source_socket = handle.socket;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_recv_step (
          router_, zlink::part_helper_internal::recv_family_router, source_socket, &helper_state,
          &first_part, &source_socket)
        != 0) {
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (first_part) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t recv_rc = recv_router_parts_once (
          &source_node_rid, &request_seq, &parts, &part_count, NULL, NULL,
          NULL);
        if (recv_rc != ZLINK_RECV_OK) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!parts || part_count == 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   request_seq);
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            export_router_recv_part_metadata_view (source_node_rid, request_seq,
                                                   source_node_rid_out_, request_seq_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }

        int stage_rc = -1;
        try {
            maybe_fail_receive_part_stage ();
            stage_rc = zlink::part_helper_internal::stage_recv_sequence (
              helper_state, zlink::part_helper_internal::recv_family_router,
              source_socket, source_node_rid, request_seq, parts, part_count,
              std::this_thread::get_id ());
        } catch (...) {
            errno = ENOMEM;
        }
        const reqrep::router_recv_metadata_tls_t &recv_metadata =
          reqrep::router_recv_metadata_tls ();
        zlink::part_helper_internal::set_recv_transport_pair (
          helper_state ? &helper_state->recv : NULL,
          recv_metadata.transport_pair_id, recv_metadata.transport_pair_generation);
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (helper_state->recv.buffered_parts.empty ()) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (take_staged_router_recv_part (
              helper_state, part_out_, has_more_out_, source_node_rid_out_,
              request_seq_out_)
            != 0) {
            revoke_router_receive_publication (handle, source_node_rid,
                                               request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    } else {
        if (take_staged_router_recv_part (
              helper_state, part_out_, has_more_out_, source_node_rid_out_,
              request_seq_out_)
            != 0) {
            revoke_staged_router_receive_publication (handle, helper_state);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_router_recv_part_v2 (
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_)
{
    if (!transport_pair_id_out_ || !transport_pair_generation_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    *transport_pair_id_out_ = 0;
    *transport_pair_generation_out_ = 0;
    const zlink_recv_result_t result = zlink_router_recv_part (
      router_, source_node_rid_out_, request_seq_out_, part_out_, has_more_out_, flags_);
    if (result == ZLINK_RECV_OK) {
        const router_recv_part_metadata_tls_t &metadata = router_recv_part_metadata_tls ();
        *transport_pair_id_out_ = metadata.transport_pair_id;
        *transport_pair_generation_out_ = metadata.transport_pair_generation;
    }
    return result;
}

zlink_recv_result_t zlink_dealer_recv_part (void *dealer_,
                                            uint8_t *message_type_out_,
                                            uint64_t *request_seq_out_,
                                            zlink_msg_t *part_out_,
                                            zlink_part_flag_t *has_more_out_,
                                            zlink_recv_flags_t flags_)
{
    if (!dealer_ || !message_type_out_ || !request_seq_out_ || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    socket_handle_t handle = as_socket_handle (dealer_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_DEALER) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_request_reply_state (handle);
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_socket_state (handle.socket);
    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (helper_state);

    auto recv_dealer_parts_once = [&] (uint8_t *message_type_out, uint64_t *request_seq_out,
                                       zlink_msg_t **parts_out,
                                       size_t *part_count_out,
                                       zlink_msg_t *terminal_part_out,
                                       bool *terminal_part_returned_out) -> zlink_recv_result_t {
        return reqrep::recv_dealer_message_direct (
                 handle, state, true, message_type_out, request_seq_out,
                 parts_out, part_count_out, static_cast<int> (flags_),
                 terminal_part_out, terminal_part_returned_out)
                 == 0
               ? ZLINK_RECV_OK
               : zlink::recv_result_internal::from_errno (errno);
    };

    if (!recv_sequence_active) {
        uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        bool terminal_part_returned = false;
        const zlink_recv_result_t recv_rc =
          recv_dealer_parts_once (&message_type, &request_seq, &parts,
                                  &part_count, part_out_,
                                  &terminal_part_returned);
        if (recv_rc != ZLINK_RECV_OK)
            return zlink::recv_result_internal::from_errno (errno);

        if (terminal_part_returned) {
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!parts || part_count == 0) {
            revoke_dealer_receive_publication (handle, request_seq);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                revoke_dealer_receive_publication (handle, request_seq);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!helper_state) {
            helper_state =
              zlink::part_helper_internal::find_or_create_socket_state (handle.socket);
            if (!helper_state) {
                zlink_multipart_close (parts, part_count);
                revoke_dealer_receive_publication (handle, request_seq);
                return zlink::recv_result_internal::from_errno (errno);
            }
        }

        int stage_rc = -1;
        try {
            maybe_fail_receive_part_stage ();
            stage_rc = zlink::part_helper_internal::stage_recv_sequence (
              helper_state, zlink::part_helper_internal::recv_family_dealer,
              handle.socket, NULL, request_seq, parts, part_count,
              std::this_thread::get_id ());
        } catch (...) {
            errno = ENOMEM;
        }
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            revoke_dealer_receive_publication (handle, request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.message_type = message_type;
        }
        if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_)
            != 0) {
            revoke_dealer_receive_publication (handle, request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        *message_type_out_ = message_type;
        *request_seq_out_ = request_seq;
        zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
        return ZLINK_RECV_OK;
    }

    bool first_part = false;
    zlink::socket_base_t *source_socket = handle.socket;
    if (zlink::part_helper_internal::prepare_recv_step (
          dealer_, zlink::part_helper_internal::recv_family_dealer, source_socket, &helper_state,
          &first_part, &source_socket)
        != 0) {
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (first_part) {
        uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        bool terminal_part_returned = false;
        const zlink_recv_result_t recv_rc =
          recv_dealer_parts_once (&message_type, &request_seq, &parts,
                                  &part_count, part_out_,
                                  &terminal_part_returned);
        if (recv_rc != ZLINK_RECV_OK) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (terminal_part_returned) {
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (
              helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }
        if (!parts || part_count == 0) {
            revoke_dealer_receive_publication (handle, request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                revoke_dealer_receive_publication (handle, request_seq);
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }

        int stage_rc = -1;
        try {
            maybe_fail_receive_part_stage ();
            stage_rc = zlink::part_helper_internal::stage_recv_sequence (
              helper_state, zlink::part_helper_internal::recv_family_dealer,
              source_socket, NULL, request_seq, parts, part_count,
              std::this_thread::get_id ());
        } catch (...) {
            errno = ENOMEM;
        }
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            revoke_dealer_receive_publication (handle, request_seq);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.message_type = message_type;
        }
    }

    if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_) != 0) {
        revoke_staged_dealer_receive_publication (handle, helper_state);
        zlink::part_helper_internal::abort_recv_step (helper_state);
        return zlink::recv_result_internal::from_errno (errno);
    }
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        *message_type_out_ = helper_state->recv.message_type;
        *request_seq_out_ = helper_state->recv.request_seq;
    }
    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}
