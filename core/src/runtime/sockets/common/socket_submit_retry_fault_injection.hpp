/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_SUBMIT_RETRY_FAULT_INJECTION_HPP_INCLUDED__
#define __ZLINK_SOCKET_SUBMIT_RETRY_FAULT_INJECTION_HPP_INCLUDED__

namespace zlink
{
namespace socket_submit_retry_fault
{
#ifdef ZLINK_BUILD_TESTS
bool pending ();
bool consume (int *err_out_);
#else
inline bool pending ()
{
    return false;
}
inline bool consume (int *)
{
    return false;
}
#endif
}
}

#endif
