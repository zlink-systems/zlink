/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/aggregate_page_read_fixture.hpp"
#include "../support/opaque_store_creation_fixture.hpp"

namespace
{
TEST (LocationAggregateAbort, InterruptedCommitPageRollbackRecoversOnAuthorityMutation)
{
    using namespace zlink::framework::test::opaque_store;
    in_memory_location_store_t provider;
    zlink::framework::test::aggregate_page_read_keeps_source_version_until_commit (provider, bytes,
                                                                                   nullptr, true);
}
}
