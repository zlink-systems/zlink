//! Option Tests – verify typed option getter/setter per socket type,
//! capability isolation, and enum/boolean surfaces.

use std::time::Duration;

use zlink::{AutoHwmProfile, Context, RidDuplicatePolicy, RoutingId, StreamSocket};

#[test]
fn context_option_auto_hwm_profile() {
    let ctx = Context::new().unwrap();
    ctx.options()
        .set_auto_hwm_profile(AutoHwmProfile::Compact)
        .unwrap();
    assert_eq!(
        ctx.options().auto_hwm_profile().unwrap(),
        AutoHwmProfile::Compact
    );
}

#[test]
fn context_option_auto_hwm_msg_unit_bytes() {
    let ctx = Context::new().unwrap();
    assert_eq!(ctx.options().auto_hwm_msg_unit_bytes().unwrap(), 0);
    ctx.options().set_auto_hwm_msg_unit_bytes(64).unwrap();
    assert_eq!(ctx.options().auto_hwm_msg_unit_bytes().unwrap(), 64);
    ctx.options().set_auto_hwm_msg_unit_bytes(0).unwrap();
    assert_eq!(ctx.options().auto_hwm_msg_unit_bytes().unwrap(), 0);
}

#[test]
fn common_option_send_high_water_mark() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let options = sock.common_options();
    options.set_send_high_water_mark(500).unwrap();
    assert_eq!(options.send_high_water_mark().unwrap(), 500);
}

#[test]
fn common_option_receive_high_water_mark() {
    let ctx = Context::new().unwrap();
    let sock = ctx.dealer_socket().unwrap();
    let options = sock.common_options();
    options.set_receive_high_water_mark(1500).unwrap();
    assert_eq!(options.receive_high_water_mark().unwrap(), 1500);
}

#[test]
fn common_option_linger_duration() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    // Typed Duration, not raw int
    sock.common_options()
        .set_linger(Duration::from_millis(200))
        .unwrap();
}

#[test]
fn common_option_tcp_keepalive_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    // Boolean, not raw int
    sock.common_options().set_tcp_keepalive(true).unwrap();
}

#[test]
fn common_option_tcp_no_delay_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
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
    let sock = ctx.router_socket().unwrap();
    sock.router_options().set_mandatory(true).unwrap();
}

#[test]
fn common_option_rid_duplicate_policy() {
    let ctx = Context::new().unwrap();
    let sock = ctx.router_socket().unwrap();
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
    let sock = ctx.router_socket().unwrap();
    sock.router_options().set_probe(true).unwrap();
}

#[test]
fn dealer_option_probe() {
    let ctx = Context::new().unwrap();
    let sock = ctx.dealer_socket().unwrap();
    sock.dealer_options().set_probe(true).unwrap();
}

#[test]
fn router_option_weight() {
    let ctx = Context::new().unwrap();
    let sock = ctx.router_socket().unwrap();
    let opts = sock.router_options();
    let initial = opts.weight().unwrap();
    opts.set_weight(50).unwrap();
    assert_eq!(opts.weight().unwrap(), 50);
    opts.set_weight(initial).unwrap();
}

#[test]
fn dealer_option_weight() {
    let ctx = Context::new().unwrap();
    let sock = ctx.dealer_socket().unwrap();
    let opts = sock.dealer_options();
    assert_eq!(opts.weight().unwrap(), 100); // default
    opts.set_weight(75).unwrap();
    assert_eq!(opts.weight().unwrap(), 75);
}

#[test]
fn pub_option_verbose() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pub_socket().unwrap();
    sock.pub_options().set_verbose(false).unwrap();
}

#[test]
fn pub_option_nodrop() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pub_socket().unwrap();
    sock.pub_options().set_no_drop(false).unwrap();
}

#[test]
fn pub_option_verboser() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pub_socket().unwrap();
    sock.pub_options().set_verboser(true).unwrap();
}

#[test]
fn stream_option_notify_boolean() {
    let ctx = Context::new().unwrap();
    let sock = ctx.stream_socket().unwrap();
    let options = sock.stream_options();
    options.set_notify(true).unwrap();
    assert!(options.notify().unwrap());
    options.set_notify(false).unwrap();
    assert!(!options.notify().unwrap());
}

#[test]
fn xpub_option_verbose() {
    let ctx = Context::new().unwrap();
    let sock = ctx.xpub_socket().unwrap();
    sock.pub_options().set_verbose(true).unwrap();
}

#[test]
fn xpub_option_verboser() {
    let ctx = Context::new().unwrap();
    let sock = ctx.xpub_socket().unwrap();
    sock.pub_options().set_verboser(true).unwrap();
}

#[test]
fn sub_option_topics_count() {
    let ctx = Context::new().unwrap();
    let sock = ctx.sub_socket().unwrap();
    sock.bind("inproc://opt-sub-topics").unwrap();
    sock.set_subscription("a").unwrap();
    sock.set_subscription("b").unwrap();
    let count = sock.sub_options().topics_count().unwrap();
    assert_eq!(count, 2);
}

#[test]
fn xsub_option_topics_count() {
    let ctx = Context::new().unwrap();
    let sock = ctx.xsub_socket().unwrap();
    sock.bind("inproc://opt-xsub-topics").unwrap();
    sock.set_subscription("a").unwrap();
    sock.set_subscription("b").unwrap();
    let count = sock.sub_options().topics_count().unwrap();
    assert_eq!(count, 2);
}

#[test]
fn dealer_and_router_expose_working_routing_id_options() {
    let ctx = Context::new().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let router = ctx.router_socket().unwrap();
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
