/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PROXY_HPP_INCLUDED__
#define __ZLINK_PROXY_HPP_INCLUDED__

namespace zlink
{
int proxy (class socket_base_t *frontend_,
           class socket_base_t *backend_,
           class socket_base_t *capture_);

#ifdef ZLINK_BUILD_TESTS
typedef void (*proxy_part_forwarded_hook_fn) (void *userdata_);
void test_set_proxy_part_forwarded_hook (proxy_part_forwarded_hook_fn hook_,
                                         void *userdata_);
void test_fail_next_proxy_destination_send ();
void test_reset_proxy_state ();
#endif
}

#endif
