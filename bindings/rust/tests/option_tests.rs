//! Option Tests – verify typed option getter/setter per socket type,
//! capability isolation, and enum/boolean surfaces.

use zlink::{AutoHwmProfile, Context, RidDuplicatePolicy, RoutingId, StreamSocket};

#[test]
fn context_option_auto_hwm_profile() {
    let ctx = Context::new().unwrap();
    ctx.options()
        .set_core_hwm_profile(AutoHwmProfile::Compact)
        .unwrap();
    assert_eq!(
        ctx.options().core_hwm_profile().unwrap(),
        AutoHwmProfile::Compact
    );
}

#[test]
fn context_core_hwm_budget_options_and_snapshot() {
    let ctx = Context::new().unwrap();
    let memory_limit = 16 * 1024 * 1024;
    let core_budget = 4 * 1024 * 1024;
    ctx.options()
        .set_core_hwm_memory_limit_bytes(memory_limit)
        .unwrap();
    ctx.options()
        .set_core_hwm_budget_bytes(core_budget)
        .unwrap();
    assert_eq!(
        ctx.options().core_hwm_memory_limit_bytes().unwrap(),
        memory_limit
    );
    assert_eq!(ctx.options().core_hwm_budget_bytes().unwrap(), core_budget);

    ctx.recalculate_auto_hwm().unwrap();
    let before = ctx.core_hwm_budget_snapshot().unwrap();
    assert_eq!(before.abi_version, 1);
    assert!(before.struct_size > 0);
    assert_eq!(before.configured_memory_limit_bytes, memory_limit);
    assert_eq!(before.configured_core_budget_bytes, core_budget);
    assert_eq!(before.effective_core_budget_bytes, core_budget);
    assert!(before.budget_planning_active());
    assert!(before.aggregate_hwm_valid());
    assert!(!before.budget_insufficient());
    assert!(!before.aggregate_overflow());
    assert_eq!(before.reserved_u64, [0; 8]);

    ctx.reset_core_hwm_budget_metrics().unwrap();
    let after = ctx.core_hwm_budget_snapshot().unwrap();
    assert_eq!(after.measurement_epoch, before.measurement_epoch + 1);
    assert_eq!(after.budget_generation, before.budget_generation);
    assert_eq!(
        after.current_accounted_bytes,
        before.current_accounted_bytes
    );
    assert_eq!(
        after.active_directional_queue_count,
        before.active_directional_queue_count
    );

    ctx.options().set_core_hwm_memory_limit_bytes(0).unwrap();
    ctx.options().set_core_hwm_budget_bytes(0).unwrap();
    assert_eq!(ctx.options().core_hwm_memory_limit_bytes().unwrap(), 0);
    assert_eq!(ctx.options().core_hwm_budget_bytes().unwrap(), 0);
}

#[test]
fn common_option_send_high_water_mark() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pair_socket().unwrap();
    let options = sock.common_options();
    options.set_send_high_water_mark(500).unwrap();
    assert_eq!(options.send_high_water_mark().unwrap(), 500);
}

#[test]
fn common_option_receive_high_water_mark() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_dealer_socket().unwrap();
    let options = sock.common_options();
    options.set_receive_high_water_mark(1500).unwrap();
    assert_eq!(options.receive_high_water_mark().unwrap(), 1500);
}

#[test]
fn common_signed_millisecond_options_preserve_core_values() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let options = sock.common_options();
    assert_eq!(options.linger().unwrap(), -1);
    for milliseconds in [-1, 0, 37, i32::MAX] {
        options.set_linger(milliseconds).unwrap();
        assert_eq!(options.linger().unwrap(), milliseconds);
        options.set_send_timeout(milliseconds).unwrap();
        assert_eq!(options.send_timeout().unwrap(), milliseconds);
        options.set_receive_timeout(milliseconds).unwrap();
        assert_eq!(options.receive_timeout().unwrap(), milliseconds);
    }
    options.set_linger(0).unwrap();
}

#[test]
fn common_option_linger_milliseconds() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pair_socket().unwrap();
    sock.common_options().set_linger(200).unwrap();
}

#[test]
fn common_option_tcp_keepalive_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pair_socket().unwrap();
    // Boolean, not raw int
    sock.common_options().set_tcp_keepalive(true).unwrap();
}

#[test]
fn common_option_tcp_no_delay_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pair_socket().unwrap();
    sock.common_options().set_tcp_no_delay(true).unwrap();
}

#[test]
fn common_option_ipv6_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.common_options().set_ipv6(false).unwrap();
}

#[test]
fn router_option_mandatory() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_router_socket().unwrap();
    sock.router_options().set_mandatory(true).unwrap();
}

#[test]
fn common_option_rid_duplicate_policy() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_router_socket().unwrap();
    sock.common_options()
        .set_rid_duplicate_policy(RidDuplicatePolicy::Handover)
        .unwrap();
    assert_eq!(
        sock.common_options().rid_duplicate_policy().unwrap(),
        RidDuplicatePolicy::Handover
    );
}

#[test]
fn router_option_probe() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_router_socket().unwrap();
    sock.router_options().set_probe(true).unwrap();
}

#[test]
fn dealer_option_probe() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_dealer_socket().unwrap();
    sock.dealer_options().set_probe(true).unwrap();
}

#[test]
fn router_option_weight() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_router_socket().unwrap();
    let opts = sock.router_options();
    let initial = opts.weight().unwrap();
    opts.set_weight(50).unwrap();
    assert_eq!(opts.weight().unwrap(), 50);
    opts.set_weight(initial).unwrap();
}

#[test]
fn dealer_option_weight() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_dealer_socket().unwrap();
    let opts = sock.dealer_options();
    assert_eq!(opts.weight().unwrap(), 100); // default
    opts.set_weight(75).unwrap();
    assert_eq!(opts.weight().unwrap(), 75);
}

#[test]
fn pub_option_verbose() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pub_socket().unwrap();
    sock.pub_options().set_verbose(false).unwrap();
}

#[test]
fn pub_option_nodrop() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pub_socket().unwrap();
    sock.pub_options().set_no_drop(false).unwrap();
}

#[test]
fn pub_option_verboser() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_pub_socket().unwrap();
    sock.pub_options().set_verboser(true).unwrap();
}

#[test]
fn stream_option_notify_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_stream_socket().unwrap();
    let options = sock.stream_options();
    options.set_notify(true).unwrap();
    assert!(options.notify().unwrap());
    options.set_notify(false).unwrap();
    assert!(!options.notify().unwrap());
}

#[test]
fn xpub_option_verbose() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_xpub_socket().unwrap();
    sock.pub_options().set_verbose(true).unwrap();
}

#[test]
fn xpub_option_verboser() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_xpub_socket().unwrap();
    sock.pub_options().set_verboser(true).unwrap();
}

#[test]
fn sub_option_topics_count() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_sub_socket().unwrap();
    sock.bind("inproc://opt-sub-topics").unwrap();
    sock.set_subscription("a").unwrap();
    sock.set_subscription("b").unwrap();
    let count = sock.sub_options().topics_count().unwrap();
    assert_eq!(count, 2);
}

#[test]
fn xsub_option_topics_count() {
    let ctx = Context::new().unwrap();
    let sock = ctx.create_xsub_socket().unwrap();
    sock.bind("inproc://opt-xsub-topics").unwrap();
    sock.set_subscription("a").unwrap();
    sock.set_subscription("b").unwrap();
    let count = sock.sub_options().topics_count().unwrap();
    assert_eq!(count, 2);
}

#[test]
fn dealer_and_router_expose_working_routing_id_options() {
    let ctx = Context::new().unwrap();
    let dealer = ctx.create_dealer_socket().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let dealer_rid = RoutingId::from(b"dealer-opt-rid");
    let router_rid = RoutingId::from(b"router-opt-rid");
    dealer.set_routing_id(&dealer_rid).unwrap();
    router.set_routing_id(&router_rid).unwrap();
    assert_eq!(dealer.routing_id().unwrap(), dealer_rid);
    assert_eq!(router.routing_id().unwrap(), router_rid);
}

#[test]
fn stream_exposes_routing_id_surface() {
    let _set = StreamSocket::set_routing_id;
    let _get = StreamSocket::routing_id;
}

// No raw option bag access – verified at compile time by absence of
// pub fn set_option / get_option on any socket type.

#[test]
fn peer_weight_range_is_validated_before_core() {
    use zlink::ConfigResult;
    let ctx = Context::new().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let dealer = ctx.create_dealer_socket().unwrap();

    router.router_options().set_weight(10_000).unwrap();
    assert_eq!(router.router_options().weight().unwrap(), 10_000);
    dealer.dealer_options().set_weight(0).unwrap();
    assert_eq!(dealer.dealer_options().weight().unwrap(), 0);

    let err = router.router_options().set_weight(10_001).unwrap_err();
    assert_eq!(err.code(), ConfigResult::InvalidArgument);
    let err = dealer.dealer_options().set_weight(10_001).unwrap_err();
    assert_eq!(err.code(), ConfigResult::InvalidArgument);
    assert_eq!(router.router_options().weight().unwrap(), 10_000);
}

#[test]
fn oversized_request_timeout_is_rejected_not_clamped() {
    use std::time::Duration;
    use zlink::ConfigResult;
    let ctx = Context::new().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let dealer = ctx.create_dealer_socket().unwrap();
    let too_long = Duration::from_millis(i32::MAX as u64 + 1);

    let err = router
        .router_options()
        .set_request_timeout(too_long)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::InvalidArgument);
    let err = dealer
        .dealer_options()
        .set_request_timeout(too_long)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::InvalidArgument);

    let max = Duration::from_millis(i32::MAX as u64);
    router.router_options().set_request_timeout(max).unwrap();
    assert_eq!(router.router_options().request_timeout().unwrap(), max);
}

#[test]
fn endpoint_over_255_bytes_fails_immediately() {
    use zlink::{BindResult, ConnectResult};
    let ctx = Context::new().unwrap();
    let socket = ctx.create_dealer_socket().unwrap();
    let long = format!("inproc://{}", "a".repeat(256));
    assert_eq!(
        socket.bind(&long).unwrap_err().code(),
        BindResult::InvalidArgument
    );
    assert_eq!(
        socket.connect(&long).unwrap_err().code(),
        ConnectResult::InvalidArgument
    );
    assert_eq!(
        socket.unbind(&long).unwrap_err().code(),
        ConnectResult::InvalidArgument
    );
    assert_eq!(
        socket.disconnect(&long).unwrap_err().code(),
        ConnectResult::InvalidArgument
    );
}
