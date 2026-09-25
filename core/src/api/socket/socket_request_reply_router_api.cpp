/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"
#include "core/scoped_msg.hpp"
#include "core/pipe.hpp"
#include "sockets/router/router.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

namespace
{
class route_source_pipe_pin_t
{
  public:
    explicit route_source_pipe_pin_t (zlink::pipe_t *pipe_) : _pipe (pipe_) {}
    ~route_source_pipe_pin_t ()
    {
        if (_pipe)
            _pipe->release_lifetime_ref ();
    }

    zlink::pipe_t *release ()
    {
        zlink::pipe_t *const pipe = _pipe;
        _pipe = NULL;
        return pipe;
    }

  private:
    zlink::pipe_t *_pipe;
};

int validate_router_recv_entry (
  void *router_, const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *reply_token_out_, const void *parts_out_, const void *count_out_,
  zlink_recv_flags_t flags_, int type_error_, socket_handle_t *handle_out_)
{
    if (!router_ || !handle_out_) {
        errno = EFAULT;
        return -1;
    }
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket)
        return -1;
    handle.socket->clear_last_recv_source_rid ();
    if (socket_type (handle) == ZLINK_CORE_SOCKET_ROUTER)
        static_cast<zlink::router_t *> (handle.socket)
          ->set_last_recv_route_generation (0);
    if (!source_node_rid_out_ || !reply_token_out_ || !parts_out_
        || !count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (validate_recv_flags (flags_) != 0)
        return -1;
    if (socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER) {
        errno = type_error_;
        return -1;
    }
    *handle_out_ = std::move (handle);
    return 0;
}

void export_router_recv_metadata_view (zlink::socket_base_t *socket_,
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

int stage_router_recv_sequence (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink::socket_base_t *source_socket_,
  const zlink_routing_id_t *source_node_rid_, uint64_t reply_token_,
  zlink_msg_t *parts_, size_t part_count_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_, uint64_t route_generation_,
  zlink::pipe_t *route_source_pipe_)
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
          transport_pair_generation_, route_generation_, route_source_pipe_);
    } catch (...) {
        errno = ENOMEM;
    }
    return rc;
}

}

zlink_recv_result_t zlink_router_recv (
  void *router_, const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *reply_token_out_, zlink_msg_t *parts_out_,
  size_t parts_capacity_, size_t *part_count_out_,
  zlink_recv_flags_t flags_)
{
    socket_handle_t handle;
    if (validate_router_recv_entry (
          router_, source_node_rid_out_, reply_token_out_, parts_out_,
          part_count_out_, flags_, ENOTSUP, &handle)
        != 0)
        return zlink::recv_result_internal::from_errno (errno);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      handle.socket->part_helper_state ();
    zlink::router_t *const router =
      static_cast<zlink::router_t *> (handle.socket);
    if (helper_state) {
        zlink::part_helper_internal::recv_record_metadata_t staged_metadata = {};
        size_t staged_part_count = 0;
        bool staged_handled = false;
        zlink_recv_result_t staged_result = ZLINK_RECV_NO_DATA;
        zlink::pipe_t *staged_pipe_pin = NULL;
        router->within_receive_turn ([&] () {
            const zlink::part_helper_internal::staged_recv_record_result_t staged_rc =
              zlink::part_helper_internal::try_take_staged_recv_record (
                helper_state, zlink::part_helper_internal::recv_family_router,
                parts_out_, parts_capacity_, &staged_part_count, &staged_metadata);
            if (staged_rc == zlink::part_helper_internal::staged_recv_record_error) {
                if (errno == ENOBUFS)
                    *part_count_out_ = staged_part_count;
                staged_result = zlink::recv_result_internal::from_errno (errno);
                staged_handled = true;
            } else if (staged_rc
                       == zlink::part_helper_internal::staged_recv_record_taken) {
                // Stage publication and route retirement also own this receive
                // turn. A taken record is current until metadata export finishes.
                staged_pipe_pin = staged_metadata.route_source_pipe;
                export_router_recv_metadata_view (
                  handle.socket,
                  staged_metadata.return_source_rid_as_null
                    ? NULL
                    : &staged_metadata.source_node_rid,
                  staged_metadata.request_seq, source_node_rid_out_, reply_token_out_,
                  staged_metadata.transport_pair_id,
                  staged_metadata.transport_pair_generation);
                *part_count_out_ = staged_part_count;
                router->set_last_recv_route_generation (
                  staged_metadata.route_generation);
                staged_result = ZLINK_RECV_OK;
                staged_handled = true;
            }
        });
        if (staged_pipe_pin)
            staged_pipe_pin->release_lifetime_ref ();
        if (staged_handled) {
            if (staged_result == ZLINK_RECV_OK)
                errno = 0;
            return staged_result;
        }
    }

    for (;;) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t reply_token = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        uint64_t transport_pair_id = 0;
        uint64_t transport_pair_generation = 0;
        uint64_t route_generation = 0;
        zlink::pipe_t *source_pipe_pin_raw = NULL;
        // The local slot preserves failure outputs and accepts uninitialized
        // caller slots without exporting a single part through TLS storage.
        zlink::scoped_msg_t terminal_part;
        bool terminal_part_returned = false;
        if (reqrep::recv_router_record (
              handle, &source_node_rid, &reply_token, &parts, &part_count,
              static_cast<int> (flags_),
              parts_capacity_ > 0 ? terminal_part.get () : NULL,
              &terminal_part_returned, &transport_pair_id,
              &transport_pair_generation, &route_generation,
              &source_pipe_pin_raw)
            != 0)
            return zlink::recv_result_internal::from_errno (errno);
        route_source_pipe_pin_t source_pipe_pin (source_pipe_pin_raw);

        if (terminal_part_returned) {
            const int adopt_rc =
              zlink_msg_adopt (&parts_out_[0], terminal_part.get ());
            errno_assert (adopt_rc == 0);
            // recv_router_record already published the socket-owned RID view.
            *source_node_rid_out_ = source_node_rid;
            *reply_token_out_ = reply_token;
            *part_count_out_ = 1;
            router->set_last_recv_route_generation (route_generation);
            errno = 0;
            return ZLINK_RECV_OK;
        }

        if (!parts || part_count == 0) {
            zlink_multipart_close (parts, part_count);
            revoke_router_receive_publication (handle, source_node_rid,
                                               reply_token);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (parts_capacity_ < part_count) {
            if (!helper_state)
                helper_state =
                  zlink::part_helper_internal::find_or_create_socket_state (
                    handle.socket);
            if (!helper_state) {
                zlink_multipart_close (parts, part_count);
                revoke_router_receive_publication (handle, source_node_rid,
                                                   reply_token);
                return zlink::recv_result_internal::from_errno (errno);
            }
            const int stage_rc = router->stage_selected_record (
              source_pipe_pin_raw, route_generation, [&] () {
                  const int staged = stage_router_recv_sequence (
                    helper_state, handle.socket, source_node_rid, reply_token,
                    parts, part_count, transport_pair_id,
                    transport_pair_generation, route_generation,
                    source_pipe_pin_raw);
                  if (staged == 0)
                      source_pipe_pin.release ();
                  return staged;
              });
            const int stage_errno = errno;
            zlink_multipart_close (parts, part_count);
            if (stage_rc != 0) {
                revoke_router_receive_publication (handle, source_node_rid,
                                                   reply_token);
                if (stage_errno == ESTALE)
                    continue;
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = stage_errno;
                return zlink::recv_result_internal::from_errno (errno);
            }
            *part_count_out_ = part_count;
            errno = ENOBUFS;
            return ZLINK_RECV_BUFFER_TOO_SMALL;
        }

        // Caller slots are uninitialized (the contract does not require init), so
        // adopt into them rather than move: adopt overwrites without inspecting or
        // closing the destination, avoiding an uninitialized read.
        for (size_t i = 0; i < part_count; ++i) {
            const int adopt_rc = zlink_msg_adopt (&parts_out_[i], &parts[i]);
            errno_assert (adopt_rc == 0);
        }
        zlink_multipart_close (parts, part_count);
        export_router_recv_metadata_view (
          handle.socket, source_node_rid, reply_token, source_node_rid_out_,
          reply_token_out_, transport_pair_id, transport_pair_generation);
        *part_count_out_ = part_count;
        router->set_last_recv_route_generation (route_generation);
        errno = 0;
        return ZLINK_RECV_OK;
    }
}

zlink_config_result_t zlink_router_routes_snapshot (
  void *router_, zlink_router_route_t *routes_out_, size_t capacity_,
  size_t *route_count_out_)
{
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket)
        return zlink::config_result_internal::from_errno (errno);
    if (socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return ZLINK_CONFIG_NOT_SUPPORTED;
    }
    if (!route_count_out_ || (!routes_out_ && capacity_ != 0)) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    zlink::router_t *const router = static_cast<zlink::router_t *> (handle.socket);
    return zlink::config_result_internal::from_rc (
      router->routes_snapshot (routes_out_, capacity_, route_count_out_));
}

uint64_t zlink_router_recv_route_generation (void *router_)
{
    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket || socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER)
        return 0;
    return static_cast<zlink::router_t *> (handle.socket)
      ->last_recv_route_generation ();
}
