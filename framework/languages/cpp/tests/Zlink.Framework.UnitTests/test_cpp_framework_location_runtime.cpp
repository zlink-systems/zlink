/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_location_store.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <stop_token>
#include <thread>

namespace
{

using zlink::framework::location_options_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_runtime_t;

class startup_owner_lease_repository_t final : public in_memory_location_repository_t
{
  public:
    enum class claim_mode_t
    {
        fail_first,
        delay_first,
        generation_exhausted,
        succeed
    };

    ~startup_owner_lease_repository_t () override
    {
        if (_delayed_claim.joinable ())
            _delayed_claim.join ();
    }

    void set_claim_mode (claim_mode_t mode) { _claim_mode = mode; }

    void delay_first_claim (std::chrono::milliseconds delay)
    {
        _claim_mode = claim_mode_t::delay_first;
        _claim_delay = delay;
    }

    void cancel_after_next_claim (std::stop_source &cancellation)
    {
        _cancellation = &cancellation;
    }

    int claim_calls () const noexcept { return _claim_calls.load (); }

    int release_calls () const noexcept { return _release_calls.load (); }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> claim_owner_lease (
      std::string owner_id,
      std::chrono::milliseconds lease_ttl) override
    {
        const auto call = _claim_calls.fetch_add (1);
        if (_claim_mode == claim_mode_t::fail_first && call == 0) {
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease provider failure"));
        }
        if (_claim_mode == claim_mode_t::delay_first && call == 0) {
            auto completion = std::make_shared<zlink::framework::detail::task_completion_source_t<
              zlink::framework::owner_lease_claim_result_t>> ();
            auto pending = completion->task ();
            _delayed_claim = std::thread (
              [this, completion, owner_id = std::move (owner_id), lease_ttl] () mutable {
                  std::this_thread::sleep_for (_claim_delay);
                  auto committed = in_memory_location_repository_t::claim_owner_lease (
                    std::move (owner_id), lease_ttl);
                  completion->complete (committed.result ());
              });
            return pending;
        }
        if (_claim_mode == claim_mode_t::generation_exhausted) {
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::success (
                zlink::framework::owner_lease_claim_result_t{
                  zlink::framework::owner_lease_generation_exhausted_t{}}));
        }
        auto result = in_memory_location_repository_t::claim_owner_lease (
          std::move (owner_id), lease_ttl);
        if (_cancellation != nullptr) {
            _cancellation->request_stop ();
            _cancellation = nullptr;
        }
        return result;
    }

    zlink::framework::task_t<zlink::framework::owner_lease_release_result_t> release_owner_lease (
      zlink::framework::location_owner_token_t token) override
    {
        _release_calls.fetch_add (1);
        return in_memory_location_repository_t::release_owner_lease (std::move (token));
    }

  private:
    claim_mode_t _claim_mode = claim_mode_t::fail_first;
    std::stop_source *_cancellation = nullptr;
    std::chrono::milliseconds _claim_delay{0};
    std::thread _delayed_claim;
    std::atomic_int _claim_calls = 0;
    std::atomic_int _release_calls = 0;
};

TEST (ZLinkFrameworkLocationRuntime, ClaimsAndReleasesOwnerLease)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    runtime.start (zlink::routing_id_t::from ("node-a"));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::owner_lease_found_t> (
        store.read_owner_lease ("owner-a")
          .result ()
          .value ()));

    runtime.stop ();
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::owner_lease_missing_t> (
        store.read_owner_lease ("owner-a")
          .result ()
          .value ()));
}

TEST (ZLinkFrameworkLocationRuntime, StartsDegradedAfterInitialOwnerLeaseClaimFailure)
{
    startup_owner_lease_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    for (int attempt = 0; attempt != 50 && !runtime.current_owner_token (); ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (runtime.current_owner_token ().has_value ());
    EXPECT_GE (store.claim_calls (), 2);

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, TimesOutInitialClaimAndReleasesLateCommitOnHeartbeat)
{
    startup_owner_lease_repository_t store;
    store.delay_first_claim (std::chrono::milliseconds (50));
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (40),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    runtime.start (zlink::routing_id_t::from ("node-a"));
    const auto startup_duration = std::chrono::steady_clock::now () - started_at;

    EXPECT_LT (startup_duration, renew_timeout + std::chrono::milliseconds (60));
    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    for (int attempt = 0; attempt != 250
                          && (!runtime.current_owner_token () || store.release_calls () == 0);
         ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    EXPECT_GE (store.release_calls (), 1);
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (runtime.current_owner_token ().has_value ());

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, RejectsInitialOwnerLeaseClaimConflict)
{
    in_memory_location_repository_t store;
    const auto existing = store.claim_owner_lease ("owner-a", std::chrono::seconds (15))
                            .result ()
                            .value ();
    ASSERT_TRUE (std::holds_alternative<zlink::framework::owner_lease_claimed_t> (existing));
    location_runtime_t runtime (store, {}, "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a")), std::runtime_error);
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, RejectsInitialOwnerLeaseClaimGenerationExhaustion)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (
      startup_owner_lease_repository_t::claim_mode_t::generation_exhausted);
    location_runtime_t runtime (store, {}, "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a")), std::runtime_error);
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, ReleasesCommittedClaimWhenStartupIsCancelled)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ()),
                  zlink::framework::framework_exception_t);
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

} // namespace
