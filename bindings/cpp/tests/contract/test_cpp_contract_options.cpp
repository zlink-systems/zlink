/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

namespace
{

template <typename T> class has_common_socket_options_facade_t
{
  private:
    template <typename U>
    static auto test (int)
      -> decltype (std::declval<U &> ().linger (),
                   std::declval<U &> ().linger (std::chrono::milliseconds (0)),
                   std::declval<U &> ().submit_retry_mode (),
                   std::declval<U &> ().submit_retry_mode (
                     zlink::submit_retry_mode_t::local_failure),
                   std::declval<U &> ().submit_retry_timeout (),
                   std::declval<U &> ().submit_retry_timeout (std::chrono::milliseconds (0)),
                   std::declval<U &> ().submit_retry_attempts (),
                   std::declval<U &> ().submit_retry_attempts (0),
                   std::declval<U &> ().send_hwm (),
                   std::declval<U &> ().send_hwm (zlink::byte_count_t::bytes (0)),
                   std::declval<U &> ().recv_hwm (),
                   std::declval<U &> ().recv_hwm (zlink::byte_count_t::bytes (0)),
                   std::declval<U &> ().last_endpoint (),
                   std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_router_socket_options_facade_t
{
  private:
    template <typename U>
    static auto
    test (int) -> decltype (std::declval<U &> ().mandatory (),
                            std::declval<U &> ().mandatory (true),
                            std::declval<U &> ().probe (),
                            std::declval<U &> ().probe (true),
                            std::declval<U &> ().connect_routing_id (),
                            std::declval<U &> ().connect_routing_id (
                              std::declval<const zlink::routing_id_t &> ()),
                            std::declval<U &> ().peer_weight (),
                            std::declval<U &> ().peer_weight (zlink::peer_weight_t::value (1)),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_dealer_socket_options_facade_t
{
  private:
    template <typename U>
    static auto
    test (int) -> decltype (std::declval<U &> ().probe (),
                            std::declval<U &> ().probe (true),
                            std::declval<U &> ().peer_weight (),
                            std::declval<U &> ().peer_weight (zlink::peer_weight_t::value (1)),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_stream_socket_options_facade_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().notify (),
                                        std::declval<U &> ().notify (true),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_pub_socket_options_facade_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().verbose (),
                                        std::declval<U &> ().verbose (true),
                                        std::declval<U &> ().verboser (),
                                        std::declval<U &> ().verboser (true),
                                        std::declval<U &> ().no_drop (),
                                        std::declval<U &> ().no_drop (true),
                                        std::declval<U &> ().manual (),
                                        std::declval<U &> ().manual (true),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_sub_socket_options_facade_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().topics_count (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_context_options_facade_t
{
  private:
    template <typename U>
    static auto test (int)
      -> decltype (std::declval<U &> ().io_threads (),
                   std::declval<U &> ().io_threads (zlink::io_thread_count_t::value (1)),
                   std::declval<U &> ().max_sockets (),
                   std::declval<U &> ().max_sockets (zlink::socket_count_t::value (1)),
                   std::declval<U &> ().max_msg_size (),
                   std::declval<U &> ().max_msg_size (zlink::byte_size_t::bytes (1)),
                   std::declval<U &> ().thread_priority (),
                   std::declval<U &> ().thread_priority (zlink::thread_priority_t::value (1)),
                   std::declval<U &> ().thread_scheduling_policy (),
                   std::declval<U &> ().thread_scheduling_policy (
                     zlink::thread_scheduling_policy_t::other),
                   std::declval<U &> ().blocky (),
                   std::declval<U &> ().blocky (true),
                   std::declval<U &> ().core_hwm_memory_limit_bytes (),
                   std::declval<U &> ().core_hwm_memory_limit_bytes (
                     zlink::byte_count_t::bytes (64)),
                   std::declval<U &> ().core_hwm_budget_bytes (),
                   std::declval<U &> ().core_hwm_budget_bytes (
                     zlink::byte_count_t::bytes (64)),
                   std::declval<U &> ().core_hwm_profile (),
                   std::declval<U &> ().core_hwm_profile (
                     zlink::auto_hwm_profile::balanced),
                   std::declval<U &> ().socket_limit (),
                   std::declval<U &> ().msg_t_size (),
                   std::declval<U &> ().add_thread_affinity (zlink::cpu_index_t::value (0)),
                   std::declval<U &> ().remove_thread_affinity (zlink::cpu_index_t::value (0)),
                   std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_legacy_auto_hwm_msg_unit_bytes_t
{
  private:
    template <typename U>
    static auto test (int)
      -> decltype (std::declval<U &> ().auto_hwm_msg_unit_bytes (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_socket_options_entry_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().options (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

static_assert (has_common_socket_options_facade_t<zlink::common_socket_options_t>::value,
               "common_socket_options_t must expose canonical methods");
static_assert (
  std::is_same<decltype (std::declval<zlink::common_socket_options_t &> ().send_hwm ()),
               zlink::byte_count_t>::value,
  "send_hwm must return a byte-specific 64-bit type");
static_assert (
  std::is_same<decltype (std::declval<zlink::common_socket_options_t &> ().recv_hwm ()),
               zlink::byte_count_t>::value,
  "recv_hwm must return a byte-specific 64-bit type");
static_assert (std::is_same<decltype (zlink::byte_count_t::bytes (0).bytes ()), uint64_t>::value,
               "byte_count_t must preserve uint64_t values");
static_assert (has_router_socket_options_facade_t<zlink::router_socket_options_t>::value,
               "router_socket_options_t must expose canonical methods");
static_assert (has_dealer_socket_options_facade_t<zlink::dealer_socket_options_t>::value,
               "dealer_socket_options_t must expose canonical methods");
static_assert (has_stream_socket_options_facade_t<zlink::stream_socket_options_t>::value,
               "stream_socket_options_t must expose canonical methods");
static_assert (has_pub_socket_options_facade_t<zlink::pub_socket_options_t>::value,
               "pub_socket_options_t must expose canonical methods");
static_assert (has_sub_socket_options_facade_t<zlink::sub_socket_options_t>::value,
               "sub_socket_options_t must expose canonical methods");
static_assert (has_context_options_facade_t<zlink::context_options_t>::value,
               "context_options_t must exist");
static_assert (!has_legacy_auto_hwm_msg_unit_bytes_t<zlink::context_options_t>::value,
               "legacy auto_hwm_msg_unit_bytes must not remain as an alias");
static_assert (
  std::is_same<decltype (std::declval<const zlink::context_t &> ().core_hwm_budget_snapshot ()),
               zlink::core_hwm_budget_snapshot_t>::value,
  "context snapshot must be returned as an immutable value");
static_assert (!std::is_default_constructible<zlink::core_hwm_budget_snapshot_t>::value,
               "callers must not initialize native snapshot ABI fields");
static_assert (
  std::is_same<decltype (std::declval<const zlink::core_hwm_budget_snapshot_t &> ()
                           == std::declval<const zlink::core_hwm_budget_snapshot_t &> ()),
               bool>::value,
  "Core HWM snapshots must preserve value equality");
static_assert (has_socket_options_entry_t<zlink::router_socket_t>::value,
               "router_socket_t must expose options()");

void test_context_options ()
{
    zlink::context_t ctx;
    zlink::context_options_t options = ctx.options ();
    options.blocky (false);
    assert (!options.blocky ());
    options.core_hwm_profile (zlink::auto_hwm_profile::compact);
    assert (options.core_hwm_profile () == zlink::auto_hwm_profile::compact);
    options.core_hwm_profile (zlink::auto_hwm_profile::throughput);
    assert (options.core_hwm_profile () == zlink::auto_hwm_profile::throughput);

    const uint64_t memory_limit = UINT64_C (16) * 1024u * 1024u;
    const uint64_t core_budget = UINT64_C (4) * 1024u * 1024u;
    options.core_hwm_memory_limit_bytes (zlink::byte_count_t::bytes (memory_limit));
    options.core_hwm_budget_bytes (zlink::byte_count_t::bytes (core_budget));
    assert (options.core_hwm_memory_limit_bytes ().bytes () == memory_limit);
    assert (options.core_hwm_budget_bytes ().bytes () == core_budget);

    ctx.recalculate_auto_hwm ();
    const zlink::core_hwm_budget_snapshot_t before = ctx.core_hwm_budget_snapshot ();
    const zlink::core_hwm_budget_snapshot_t before_copy = before;
    assert (before_copy == before);
    assert (before.abi_version () == 1u);
    assert (before.struct_size () > 0u);
    assert (before.configured_memory_limit_bytes () == memory_limit);
    assert (before.configured_core_budget_bytes () == core_budget);
    assert (before.effective_core_budget_bytes () == core_budget);
    assert (before.budget_planning_active ());
    assert (before.aggregate_hwm_valid ());
    assert (!before.budget_insufficient ());
    assert (!before.aggregate_overflow ());
    assert (before.flags () != 0u);
    assert ((before.reserved_u64 () == std::array<uint64_t, 8>{}));

    ctx.reset_core_hwm_budget_metrics ();
    const zlink::core_hwm_budget_snapshot_t after = ctx.core_hwm_budget_snapshot ();
    assert (after != before);
    assert (after.measurement_epoch () == before.measurement_epoch () + 1u);
    assert (after.budget_generation () == before.budget_generation ());
    assert (after.current_accounted_bytes () == before.current_accounted_bytes ());
    assert (after.active_directional_queue_count ()
            == before.active_directional_queue_count ());

    options.core_hwm_memory_limit_bytes (zlink::byte_count_t::bytes (0));
    options.core_hwm_budget_bytes (zlink::byte_count_t::bytes (0));
    assert (options.core_hwm_memory_limit_bytes ().bytes () == 0u);
    assert (options.core_hwm_budget_bytes ().bytes () == 0u);

    options.io_threads (zlink::io_thread_count_t::value (2));
    assert (options.io_threads ().value () == 2);
    options.max_sockets (zlink::socket_count_t::value (128));
    assert (options.max_sockets ().value () == 128);
    try {
        options.add_thread_affinity (zlink::cpu_index_t::value (0));
        options.remove_thread_affinity (zlink::cpu_index_t::value (0));
    }
    catch (const zlink::config_error_t &err) {
        assert (err.result () == zlink::config_result_t::not_supported);
    }
    assert (options.socket_limit ().value () >= options.max_sockets ().value ());
    assert (options.msg_t_size ().bytes () > 0);
}

void test_socket_common_and_router_options ()
{
    zlink::context_t ctx;
    ctx.options ().auto_hwm_enabled (false);
    zlink::router_socket_t router (ctx);
    zlink::common_socket_options_t common = router.options ();
    assert (common.send_hwm ().bytes () == UINT64_C (4096000));
    assert (common.recv_hwm ().bytes () == UINT64_C (4096000));
    const uint64_t boundary = UINT64_MAX;
    common.send_hwm (zlink::byte_count_t::bytes (boundary));
    common.recv_hwm (zlink::byte_count_t::bytes (0));
    assert (common.send_hwm ().bytes () == boundary);
    assert (common.recv_hwm ().bytes () == 0);
    common.linger (std::chrono::milliseconds (0));
    assert (common.linger () == std::chrono::milliseconds (0));
    assert (common.submit_retry_mode () == zlink::submit_retry_mode_t::off);
    assert (common.submit_retry_timeout () == std::chrono::milliseconds (0));
    assert (common.submit_retry_attempts () == 0);
    common.submit_retry_mode (zlink::submit_retry_mode_t::local_failure);
    common.submit_retry_timeout (std::chrono::milliseconds (42));
    common.submit_retry_attempts (2);
    assert (common.submit_retry_mode () == zlink::submit_retry_mode_t::local_failure);
    assert (common.submit_retry_timeout () == std::chrono::milliseconds (42));
    assert (common.submit_retry_attempts () == 2);

    zlink::stream_socket_t stream (ctx);
    zlink::stream_socket_options_t stream_options = stream.options ();
    stream_options.notify (true);
    assert (stream_options.notify ());

    const std::string rid_text = "router-alpha";
    const zlink::routing_id_t expected_routing_id = zlink::routing_id_t::from (
      reinterpret_cast<const uint8_t *> (rid_text.data ()), rid_text.size ());
    router.set_routing_id (expected_routing_id);
    zlink::routing_id_t routing_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("x"), 1);
    router.get_routing_id (routing_id);
    assert (routing_id.to_bytes () == std::vector<uint8_t> (rid_text.begin (), rid_text.end ()));
}

} // namespace

int main ()
{
    test_context_options ();
    test_socket_common_and_router_options ();
    return 0;
}
