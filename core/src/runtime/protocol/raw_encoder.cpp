/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/raw_encoder.hpp"
#include "core/msg.hpp"

zlink::raw_encoder_t::raw_encoder_t (size_t bufsize_) : encoder_base_t<raw_encoder_t> (bufsize_)
{
    next_step (NULL, 0, &raw_encoder_t::raw_message_ready, true);
}

zlink::raw_encoder_t::~raw_encoder_t ()
{
}

void zlink::raw_encoder_t::raw_message_ready ()
{
    next_step (in_progress ()->data (), in_progress ()->size (), &raw_encoder_t::raw_message_ready,
               true);
}
