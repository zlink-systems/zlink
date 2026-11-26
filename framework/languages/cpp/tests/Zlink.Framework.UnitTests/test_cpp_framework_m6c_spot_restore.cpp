/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/committed_authority_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

namespace
{

class fail_first_restore_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit fail_first_restore_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override {}

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        ++create_count;
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override
    {
        ++initialize_count;
        co_return;
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    static inline int factory_count = 0;
    static inline int restore_count = 0;
    static inline int create_count = 0;
    static inline int initialize_count = 0;
    static inline std::vector<std::byte> restored_payload;

  private:
    zlink::framework::spot_context_t _context;
};

class fail_first_restore_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<fail_first_restore_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (fail_first_restore_spot_t &,
                                                              std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }

    zlink::framework::task_t<void>
    restore (fail_first_restore_spot_t &, std::vector<std::byte> payload, std::stop_token) override
    {
        if (++fail_first_restore_spot_t::restore_count == 1) {
            throw std::runtime_error ("expected first restore failure");
        }
        fail_first_restore_spot_t::restored_payload = std::move (payload);
        co_return;
    }
};

class concurrent_restore_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit concurrent_restore_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
        ++factory_count;
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    static inline std::atomic_int factory_count{0};
    static inline std::atomic_int restore_count{0};
    static inline std::mutex restore_mutex;
    static inline std::condition_variable restore_condition;
    static inline bool restore_entered = false;
    static inline bool release_restore = false;
    static inline std::vector<std::byte> restored_payload;

  private:
    zlink::framework::spot_context_t _context;
};

class concurrent_restore_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<concurrent_restore_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (concurrent_restore_spot_t &,
                                                              std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }

    zlink::framework::task_t<void>
    restore (concurrent_restore_spot_t &, std::vector<std::byte> payload, std::stop_token) override
    {
        concurrent_restore_spot_t::restore_count.fetch_add (1);
        std::unique_lock lock (concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::restore_entered = true;
        concurrent_restore_spot_t::restore_condition.notify_all ();
        concurrent_restore_spot_t::restore_condition.wait (
          lock, [] { return concurrent_restore_spot_t::release_restore; });
        concurrent_restore_spot_t::restored_payload = std::move (payload);
        co_return;
    }
};

class single_capture_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<fail_first_restore_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (fail_first_restore_spot_t &,
                                                              std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }
    zlink::framework::task_t<void>
    restore (fail_first_restore_spot_t &, std::vector<std::byte> payload, std::stop_token) override
    {
        ++fail_first_restore_spot_t::restore_count;
        fail_first_restore_spot_t::restored_payload = std::move (payload);
        co_return;
    }
};

/* A relocation adapter has exactly one capture()/restore() path — pin that
 * restore_spot_relocation_state runs it byte-identically end to end. */

} // namespace

void test_spot_restore_stages_before_publication (test_context_t &test)
{
    fail_first_restore_spot_t::factory_count = 0;
    fail_first_restore_spot_t::restore_count = 0;
    fail_first_restore_spot_t::create_count = 0;
    fail_first_restore_spot_t::initialize_count = 0;
    fail_first_restore_spot_t::restored_payload.clear ();

    zlink::framework::spot_node_builder_t builder;
    builder.add_spot_factory<fail_first_restore_spot_t> (
      "restored-spot",
      [] (zlink::framework::spot_context_t context) {
          ++fail_first_restore_spot_t::factory_count;
          return std::make_shared<fail_first_restore_spot_t> (std::move (context));
      },
      [] (auto &factory) {
          factory.template preserve_state_with<fail_first_restore_adapter_t> ();
      });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder);
    const frozen_object_state_t frozen{.owner = {.kind = object_kind_t::user_spot,
                                                 .key = "staged-spot",
                                                 .object_generation = 7,
                                                 .authority_owner_generation = 9,
                                                 .mesh_name = "mesh",
                                                 .node_id = "source"},
                                       .stable_type = "restored-spot",
                                       .application_state = {0xca, 0xfe},
                                       .pending_application = {},
                                       .timers = {}};
    const object_ref_t target{.kind = object_kind_t::user_spot,
                              .key = "staged-spot",
                              .object_generation = 7,
                              .authority_owner_generation = 10,
                              .mesh_name = "mesh",
                              .node_id = "target"};

    auto committed_target = target;
    committed_target.authority_owner_generation = 12;
    runtime.bind_relocation_authority (
      std::make_shared<zlink::framework::test::committed_authority_fixture_t> (committed_target));

    auto oversized = frozen;
    oversized.application_state.assign (64u * 1024u * 1024u + 1u, 0x5a);
    test.require (!runtime.restore_spot_relocation_state (oversized, target).result ().value ()
                    && fail_first_restore_spot_t::factory_count == 0
                    && fail_first_restore_spot_t::restore_count == 0,
                  "oversized state must be rejected before Spot materialization");

    test.require (!runtime.restore_spot_relocation_state (frozen, target).result ().value ()
                    && !runtime.find_spot (zlink::framework::spot_id_t ("staged-spot"))
                    && fail_first_restore_spot_t::factory_count == 1
                    && fail_first_restore_spot_t::create_count == 0
                    && fail_first_restore_spot_t::initialize_count == 0,
                  "failed restore must discard the private Spot before publication");

    test.require (runtime.restore_spot_relocation_state (frozen, target).result ().value ()
                    && runtime.find_spot (zlink::framework::spot_id_t ("staged-spot"))
                    && fail_first_restore_spot_t::factory_count == 2
                    && fail_first_restore_spot_t::restore_count == 2
                    && fail_first_restore_spot_t::create_count == 0
                    && fail_first_restore_spot_t::initialize_count == 0
                    && fail_first_restore_spot_t::restored_payload
                         == std::vector<std::byte>{std::byte{0xca}, std::byte{0xfe}},
                  "restore retry must retain a fresh uninitialized target before authority commit");
    test.require (runtime.commit_relocation_materialization ({target})
                    && fail_first_restore_spot_t::initialize_count == 1,
                  "confirmed target commit must initialize the restored incarnation once");
    if (runtime.find_spot (zlink::framework::spot_id_t ("staged-spot"))) {
        const auto owner = runtime.weak_state ().lock ();
        const auto context = owner->lane
                               .run ([owner] {
                                   return zlink::framework::detail::spot_context_access_t::state (
                                     owner->spot_contexts_by_id.at ("staged-spot"));
                               })
                               .get ();
        test.require (
          context->authority_owner_generation == committed_target.authority_owner_generation,
          "Spot admission must use the committed authority, not its staging prediction");
        context
          ->run_serial_task ("after-restored-target-commit",
                             [] () -> zlink::framework::task_t<void> { co_return; })
          .result ()
          .value ();
    }
    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

void test_concurrent_spot_restore_owns_one_reservation (test_context_t &test)
{
    concurrent_restore_spot_t::factory_count.store (0);
    concurrent_restore_spot_t::restore_count.store (0);
    {
        std::lock_guard lock (concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::restore_entered = false;
        concurrent_restore_spot_t::release_restore = false;
        concurrent_restore_spot_t::restored_payload.clear ();
    }
    zlink::framework::spot_node_builder_t builder;
    builder.add_spot_factory<concurrent_restore_spot_t> (
      "concurrent-spot",
      [] (zlink::framework::spot_context_t context) {
          return std::make_shared<concurrent_restore_spot_t> (std::move (context));
      },
      [] (auto &factory) {
          factory.template preserve_state_with<concurrent_restore_adapter_t> ();
      });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder);
    const frozen_object_state_t frozen{.owner = {.kind = object_kind_t::user_spot,
                                                 .key = "concurrent-id",
                                                 .object_generation = 1,
                                                 .authority_owner_generation = 1,
                                                 .mesh_name = "mesh",
                                                 .node_id = "source"},
                                       .stable_type = "concurrent-spot",
                                       .application_state = {1},
                                       .pending_application = {},
                                       .timers = {}};
    const object_ref_t target{.kind = object_kind_t::user_spot,
                              .key = "concurrent-id",
                              .object_generation = 1,
                              .authority_owner_generation = 2,
                              .mesh_name = "mesh",
                              .node_id = "target"};

    bool first = false;
    std::thread owner (
      [&] { first = runtime.restore_spot_relocation_state (frozen, target).result ().value (); });
    {
        std::unique_lock lock (concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::restore_condition.wait (
          lock, [] { return concurrent_restore_spot_t::restore_entered; });
    }
    std::atomic_bool contender_started{false};
    bool contender_observed_existing = false;
    std::thread contender ([&] {
        contender_started.store (true);
        const auto activated = runtime.get_or_create_spot (
          "concurrent-spot", zlink::framework::spot_id_t ("concurrent-id"));
        contender_observed_existing =
          activated.state == zlink::framework::spot_create_state_t::existing;
    });
    while (!contender_started.load ())
        std::this_thread::yield ();
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    test.require (concurrent_restore_spot_t::factory_count.load () == 1
                    && concurrent_restore_spot_t::restore_count.load () == 1,
                  "normal activation must wait on the relocation reservation");
    {
        std::lock_guard lock (concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::release_restore = true;
    }
    concurrent_restore_spot_t::restore_condition.notify_all ();
    owner.join ();
    contender.join ();

    test.require (
      first && contender_observed_existing && concurrent_restore_spot_t::factory_count.load () == 1
        && concurrent_restore_spot_t::restore_count.load () == 1
        && concurrent_restore_spot_t::restored_payload == std::vector<std::byte>{std::byte{1}}
        && runtime.find_spot (zlink::framework::spot_id_t ("concurrent-id")),
      "one SpotId reservation must own materialization and publication");
    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

void test_relocation_adapter_single_capture_restore_path (test_context_t &test)
{
    fail_first_restore_spot_t::factory_count = 0;
    fail_first_restore_spot_t::restore_count = 0;
    fail_first_restore_spot_t::create_count = 0;
    fail_first_restore_spot_t::initialize_count = 0;
    fail_first_restore_spot_t::restored_payload.clear ();

    zlink::framework::spot_node_builder_t builder;
    builder.add_spot_factory<fail_first_restore_spot_t> (
      "single-capture-spot",
      [] (zlink::framework::spot_context_t context) {
          ++fail_first_restore_spot_t::factory_count;
          return std::make_shared<fail_first_restore_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.template preserve_state_with<single_capture_adapter_t> (); });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder);

    const object_ref_t source_spot{.kind = object_kind_t::user_spot,
                                   .key = "single-capture-spot-id",
                                   .object_generation = 1,
                                   .authority_owner_generation = 1,
                                   .mesh_name = "mesh",
                                   .node_id = "source"};

    const frozen_object_state_t frozen{.owner = source_spot,
                                       .stable_type = "single-capture-spot",
                                       .application_state = {0xca, 0xfe},
                                       .pending_application = {},
                                       .timers = {}};
    const object_ref_t target{.kind = object_kind_t::user_spot,
                              .key = "single-capture-spot-id",
                              .object_generation = 1,
                              .authority_owner_generation = 2,
                              .mesh_name = "mesh",
                              .node_id = "target"};
    test.require (runtime.restore_spot_relocation_state (frozen, target).result ().value ()
                    && fail_first_restore_spot_t::restore_count == 1
                    && fail_first_restore_spot_t::restored_payload
                         == std::vector<std::byte>{std::byte{0xca}, std::byte{0xfe}},
                  "a relocation adapter must restore via its single capture/restore path, "
                  "byte-identical to the captured application state");
    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

} // namespace m6c_test
