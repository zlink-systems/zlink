/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/zmp_encoder.hpp"
#include "core/msg.hpp"
#include "protocol/zmp_data_header.hpp"

zlink::zmp_encoder_t::zmp_encoder_t (size_t bufsize_) :
    encoder_base_t<zmp_encoder_t> (bufsize_),
    _application_multipart_in_progress (false)
{
    next_step (NULL, 0, &zmp_encoder_t::header_ready, true);
}

zlink::zmp_encoder_t::~zmp_encoder_t ()
{
}

void zlink::zmp_encoder_t::header_ready ()
{
    const msg_t *msg = in_progress ();
    size_t header_size = 0;
    if (!build_header (*msg, _tmp_buf, sizeof (_tmp_buf), header_size)) {
        //  Consume a rejected frame without emitting a partial header/body.
        next_step (NULL, 0, &zmp_encoder_t::header_ready, true);
        return;
    }

    next_step (_tmp_buf, header_size, &zmp_encoder_t::body_ready, false);
}

bool zlink::zmp_encoder_t::build_header (const msg_t &msg_,
                                         unsigned char *buffer_,
                                         size_t buffer_size_,
                                         size_t &header_size_)
{
    const unsigned char special_flags =
      msg_t::command | msg_t::routing_id | msg_t::subscribe | msg_t::cancel;
    const bool special_frame = (msg_.flags () & special_flags) != 0;

    bool has_request_reply = false;
    if (!build_zmp_data_header (msg_, buffer_, buffer_size_, header_size_,
                                has_request_reply))
        return false;

    if (_application_multipart_in_progress
        && (special_frame || has_request_reply)) {
        header_size_ = 0;
        errno = EINVAL;
        return false;
    }

    if (!special_frame)
        _application_multipart_in_progress =
          (msg_.flags () & msg_t::more) != 0;
    return true;
}

void zlink::zmp_encoder_t::body_ready ()
{
    next_step (in_progress ()->data (), in_progress ()->size (), &zmp_encoder_t::header_ready,
               true);
}
