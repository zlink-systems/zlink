/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/discarded_authority_fixture.hpp"

namespace
{
class OwnerTokenReleaseWait : public DiscardedAuthority
{
  protected:
    bool release (const char *id,
                  const authority_snapshot_t &before,
                  const location_owner_token_t &target)
    {
        return repository->cleanup_discarded (actor_authority_key (id), before, target)
          .result ()
          .value ();
    }
};

TEST_P (OwnerTokenReleaseWait, SharesElapsedWaitAcrossRowsAndSeparatesGenerations)
{
    const auto first = create ("first");
    const auto second = create ("second");
    const auto next = replace_owner ();
    owner = next;
    const auto new_generation = create ("new-generation");
    const auto target = replace_owner ();
    ASSERT_EQ (next.lease_generation, first.owner.lease_generation + 1);

    repository->configure_reclaim_margin (5s);
    EXPECT_FALSE (release ("first", first, target));
    clock.advance (14999ms);
    EXPECT_FALSE (release ("first", first, target));
    clock.advance (1ms);
    EXPECT_TRUE (release ("first", first, target));
    EXPECT_TRUE (release ("second", second, target));
    EXPECT_FALSE (release ("new-generation", new_generation, target));
    clock.advance (15s);
    EXPECT_TRUE (release ("new-generation", new_generation, target));
}

TEST_P (OwnerTokenReleaseWait, StartsFreshForANewRepository)
{
    const auto first = create ("first");
    auto untouched = create ("untouched");
    auto target = replace_owner ();
    repository->configure_reclaim_margin (5s);
    ASSERT_FALSE (release ("first", first, target));
    clock.advance (15s);

    provider_location_repository_t restarted_provider{opaque};
    in_memory_location_repository_t restarted_memory;
    if (GetParam ()) {
        repository = &restarted_provider;
    } else {
        // A new in-memory repository owns its own rows. Seed the same owner and lease history.
        repository = &restarted_memory;
        repository->configure_reclaim_margin (0ms);
        seed_owner ();
        untouched = create ("untouched");
        target = replace_owner ();
    }
    clock.bind (*repository);
    repository->configure_reclaim_margin (5s);
    EXPECT_FALSE (release ("untouched", untouched, target));
    clock.advance (15s);
    EXPECT_TRUE (release ("untouched", untouched, target));
}

INSTANTIATE_TEST_SUITE_P (Backends, OwnerTokenReleaseWait, ::testing::Bool ());
} // namespace
