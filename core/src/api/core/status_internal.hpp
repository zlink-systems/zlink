/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_STATUS_INTERNAL_HPP_INCLUDED__
#define __ZLINK_STATUS_INTERNAL_HPP_INCLUDED__

namespace zlink
{
namespace status_internal
{
inline bool from_rc (int rc_)
{
    return rc_ == 0;
}
}
}

#endif
