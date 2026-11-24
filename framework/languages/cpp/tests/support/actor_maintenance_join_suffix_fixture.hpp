/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

namespace zlink::framework::test::actor_maintenance_join
{
void next_join_suffix (bool remote,
                       bool request = false,
                       bool restore_source = false,
                       bool native_initiator = false,
                       bool hold_native_suffix = false);
} // namespace zlink::framework::test::actor_maintenance_join
