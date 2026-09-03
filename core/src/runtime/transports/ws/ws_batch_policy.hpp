/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__
#define __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__

namespace zlink
{
namespace ws_batch_policy
{
// One encoder batch becomes one Beast binary write. Keep enough small ZMP
// records in that transport-owned write without changing other transports'
// engine batching policy.
inline int zmp_send_batch_size ()
{
    return 16 * 1024;
}
}
}

#endif
