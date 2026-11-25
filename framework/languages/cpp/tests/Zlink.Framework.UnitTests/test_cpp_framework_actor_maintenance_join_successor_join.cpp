/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_observations.hpp"
#include "../support/actor_maintenance_join_suffix_fixture.hpp"
#include <gtest/gtest.h>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (ActorMaintenanceJoin, NextLocalJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (false);
}
TEST (ActorMaintenanceJoin, NextRemoteJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (true);
}

TEST (ActorMaintenanceJoin, NextLocalJoinPreservesSuffixRequestReply)
{
    next_join_suffix (false, true);
}
TEST (ActorMaintenanceJoin, NextRemoteJoinPreservesSuffixRequestReply)
{
    next_join_suffix (true, true);
}

TEST (ActorMaintenanceJoin, FailedRemoteJoinRestoresUnstartedSuffixOnce)
{
    next_join_suffix (true, false, true);
}

TEST (ActorMaintenanceJoin, FailedRemoteJoinRestoresUnstartedSuffixRequestReply)
{
    next_join_suffix (true, true, true);
}

TEST (ActorMaintenanceJoin, NativeInitialLocalJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (false, false, false, true);
}
TEST (ActorMaintenanceJoin, NativeInitialRemoteJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (true, false, false, true);
}
} // namespace zlink::framework::test::actor_maintenance_join
