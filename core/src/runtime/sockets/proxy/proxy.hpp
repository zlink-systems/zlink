/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PROXY_HPP_INCLUDED__
#define __ZLINK_PROXY_HPP_INCLUDED__

namespace zlink
{
int proxy (class socket_base_t *frontend_,
           class socket_base_t *backend_,
           class socket_base_t *capture_);

#ifdef ZLINK_BUILD_TESTS
void test_fail_next_proxy_destination_send ();
void test_reset_proxy_state ();
#endif
}

#endif
