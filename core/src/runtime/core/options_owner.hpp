/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_OPTIONS_OWNER_HPP_INCLUDED__
#define __ZLINK_OPTIONS_OWNER_HPP_INCLUDED__

#include "zlink.h"

namespace zlink
{
// Owner map for the shared options_t storage bag.
// This keeps the storage layout stable while making the validation/apply
// owner explicit in code. Socket-specific options remain behind their socket
// implementations and should not be routed through the central options_t bag.
enum options_owner_t
{
    options_owner_unknown = 0,
    options_owner_core_socket,
    options_owner_transport_network,
    options_owner_protocol_metadata,
    options_owner_socket_specific
};

// Shared-bag fields that are not exposed as direct getsockopt/setsockopt
// numbers still need an explicit owner so later additions do not silently
// turn options_t back into a central interpretation hub.
enum options_bag_field_t
{
    options_bag_field_monitor_event_version = 0,
    options_bag_field_hello_msg,
    options_bag_field_disconnect_msg,
    options_bag_field_hiccup_msg,
    options_bag_field_busy_poll
};

// Internal owner lookup for options that are still routed through the shared
// options_t bag. Options handled by ctx/runtime or socket implementations before
// reaching options_t are intentionally outside this lookup.
options_owner_t option_owner_of (int option_);
options_owner_t option_owner_of_bag_field (options_bag_field_t field_);

// Public option-surface owner helpers. These keep socket-specific options
// explicit even when their internal numeric ids overlap with shared-bag
// options (for example XPUB manual last value and TLS verify).
options_owner_t common_option_owner_of (zlink_option_t option_);
options_owner_t router_option_owner_of (zlink_router_option_t option_);
options_owner_t dealer_option_owner_of (zlink_dealer_option_t option_);
options_owner_t stream_option_owner_of (zlink_stream_option_t option_);
options_owner_t pub_option_owner_of (int option_);
options_owner_t sub_option_owner_of (int option_);

const char *option_owner_name (options_owner_t owner_);
}

#endif
