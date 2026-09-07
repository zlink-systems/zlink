/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/session_base.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "protocol/zmp_decoder.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"

int zlink::session_base_t::pull_msg (msg_t *msg_)
{
    while (true) {
        if (!_pipe || !_pipe->read (msg_)) {
            errno = EAGAIN;
            return -1;
        }
        const bool more = (msg_->flags () & msg_t::more) != 0;
        const uint64_t stamped = msg_->transport_connection_id ();
        const uint64_t current = _pipe->get_transport_connection_id ();
        if (_dropping_stale_transport_message
            || (stamped != 0 && stamped != current)) {
            _dropping_stale_transport_message = more;
            const int close_rc = msg_->close ();
            errno_assert (close_rc == 0);
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            continue;
        }
        _incomplete_in = more;
        return 0;
    }
}

int zlink::session_base_t::push_msg (msg_t *msg_)
{
    return push_msg_internal (msg_, NULL);
}

int zlink::session_base_t::push_msg_with_decoder_reservation (
  msg_t *msg_, void **reservation_)
{
    return push_msg_internal (msg_, reservation_);
}

void zlink::session_base_t::configure_zmp_decoder (
  zmp_decoder_t *decoder_)
{
    if (!decoder_)
        return;
    decoder_->set_frame_admission_handler (
      &session_base_t::reserve_decoder_frame,
      &session_base_t::release_decoder_frame, this);
}

int zlink::session_base_t::reserve_decoder_frame (
  void *subject_, uint32_t payload_bytes_, unsigned char msg_flags_,
  void **reservation_out_)
{
    if (!subject_ || !reservation_out_) {
        errno = EFAULT;
        return -1;
    }
    *reservation_out_ = NULL;
    session_base_t *const self = static_cast<session_base_t *> (subject_);
    if (!self->_pipe) {
        errno = ETERM;
        return -1;
    }
    decoder_frame_reservation_t *reservation = NULL;
    const bool track_multipart =
      (msg_flags_ & msg_t::routing_id) == 0
      || self->options.recv_routing_id;
    const int rc = self->_pipe->reserve_inbound_decoder_frame (
      payload_bytes_, msg_flags_, track_multipart,
      &self->_decoder_frame_reservation, &reservation);
    *reservation_out_ = reservation;
    return rc;
}

void zlink::session_base_t::release_decoder_frame (
  void *subject_, void *reservation_)
{
    decoder_frame_reservation_t *reservation =
      static_cast<decoder_frame_reservation_t *> (reservation_);
    session_base_t *const self = static_cast<session_base_t *> (subject_);
    if (self && self->_pipe)
        self->_pipe->release_decoder_frame_reservation (&reservation);
    else if (reservation)
        reservation->reset ();
}

int zlink::session_base_t::push_msg_internal (msg_t *msg_,
                                              void **reservation_)
{
    if (_pipe)
        msg_->set_transport_connection_id (
          _pipe->get_transport_connection_id ());
    if ((msg_->flags () & msg_t::command) && !msg_->is_subscribe () && !msg_->is_cancel ()) {
        zlink_assert (!reservation_ || !*reservation_);
        if (_socket) {
            const int control_rc = _socket->peer_command_from_io (msg_, _pipe);
            if (control_rc < 0)
                return -1;
            if (control_rc > 0) {
                const int rc = msg_->close ();
                errno_assert (rc == 0);
                return msg_->init ();
            }
        }
        return 0;
    }


    if (_pipe && reservation_ && *reservation_) {
        decoder_frame_reservation_t *decoder_reservation =
          static_cast<decoder_frame_reservation_t *> (*reservation_);
        const int write_rc = _pipe->write_reserved_decoder_frame (
          msg_, &decoder_reservation);
        *reservation_ = decoder_reservation;
        if (write_rc == 0) {
            const int rc = msg_->init ();
            errno_assert (rc == 0);
            return 0;
        }
        return -1;
    }

    if (_pipe && _pipe->write (msg_)) {
        const int rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    errno = EAGAIN;
    return -1;
}

void zlink::session_base_t::flush ()
{
    if (_pipe)
        _pipe->flush ();
}

void zlink::session_base_t::rollback ()
{
    if (_pipe)
        _pipe->rollback ();
}

void zlink::session_base_t::clean_pipes ()
{
    zlink_assert (_pipe != NULL);

    _pipe->rollback ();
    _pipe->flush ();

    while (_incomplete_in) {
        msg_t msg;
        int rc = msg.init ();
        errno_assert (rc == 0);
        rc = pull_msg (&msg);
        if (rc != 0) {
            const int saved_errno = errno;
            rc = msg.close ();
            errno_assert (rc == 0);
            if (saved_errno == EAGAIN) {
                _incomplete_in = false;
                break;
            }
            errno_assert (false);
        }
        rc = msg.close ();
        errno_assert (rc == 0);
    }
}
