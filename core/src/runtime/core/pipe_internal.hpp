/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PIPE_INTERNAL_HPP_INCLUDED__
#define __ZLINK_PIPE_INTERNAL_HPP_INCLUDED__

#include "core/pipe.hpp"

namespace zlink
{
namespace pipe_detail
{
extern const unsigned char head_reclassify_idle;
extern const unsigned char head_reclassify_armed;
extern const unsigned char head_reclassify_queued;

bool consume_if_delimiter (const msg_t &msg_, void *);
void pipe_debug_log (const pipe_t *pipe_, const char *phase_, int state_,
                     bool delay_, const char *identifier_);
}
}

#endif
