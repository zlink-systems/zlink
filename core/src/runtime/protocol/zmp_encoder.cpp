/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/zmp_encoder.hpp"
#include "core/msg.hpp"
#include "protocol/wire.hpp"

zlink::zmp_encoder_t::zmp_encoder_t (size_t bufsize_) : encoder_base_t<zmp_encoder_t> (bufsize_)
{
    next_step (NULL, 0, &zmp_encoder_t::header_ready, true);
}

zlink::zmp_encoder_t::~zmp_encoder_t ()
{
}

void zlink::zmp_encoder_t::header_ready ()
{
    const msg_t *msg = in_progress ();
    const size_t size = msg->size ();
    const unsigned char msg_flags = msg->flags ();

    unsigned char flags = 0;
    if (msg_flags != 0) {
        if (msg_flags & msg_t::more)
            flags |= zmp_flag_more;
        if (msg_flags & msg_t::command)
            flags |= zmp_flag_control;
        if (msg_flags & msg_t::routing_id)
            flags |= zmp_flag_identity;

        const unsigned char cmd_type = msg_flags & CMD_TYPE_MASK;
        if (cmd_type == msg_t::subscribe)
            flags |= zmp_flag_subscribe;
        else if (cmd_type == msg_t::cancel)
            flags |= zmp_flag_cancel;
    }

    _tmp_buf[0] = zmp_magic;
    _tmp_buf[1] = zmp_version;
    _tmp_buf[2] = flags;
    _tmp_buf[3] = 0;
    put_uint32 (_tmp_buf + 4, static_cast<uint32_t> (size));

    next_step (_tmp_buf, zmp_header_size, &zmp_encoder_t::body_ready, false);
}

void zlink::zmp_encoder_t::body_ready ()
{
    next_step (in_progress ()->data (), in_progress ()->size (), &zmp_encoder_t::header_ready,
               true);
}
