/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ROUTED_SUBMIT_TARGET_HPP_INCLUDED__
#define __ZLINK_ROUTED_SUBMIT_TARGET_HPP_INCLUDED__

#include <zlink.h>

namespace zlink
{
// Socket-runtime selection snapshot. Public completion submissions carry only
// a logical routing ID; the runtime may refresh the physical pair fields before
// admission.
struct routed_submit_target_t
{
    zlink_routing_id_t peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
};
}

// Keep the existing runtime spelling local to the implementation while the
// socket classes are migrated independently of the removed C surface.
typedef zlink::routed_submit_target_t zlink_routed_submit_target_t;
typedef uint64_t zlink_send_op_id_t;

#endif
