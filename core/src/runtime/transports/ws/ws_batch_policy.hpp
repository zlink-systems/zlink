/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__
#define __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__

namespace zlink
{
namespace ws_batch_policy
{
// One encoder batch becomes one Beast binary write. Use the same bounded
// capacity for that batch and the client masking scratch, so a prepared
// batch does not acquire a second transport-owned split point.
inline int zmp_send_batch_size ()
{
    return 128 * 1024;
}
}
}

#endif
