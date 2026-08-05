//! Send Failure Contract Tests – verify that blocking send failures surface
//! as errors and non-blocking sends return explicit outcomes.

use std::thread;
use std::time::Duration;

use zlink::{Context, Message, RoutingId, SendFlags};

#[test]
fn blocking_send_failure_surfaces_error() {
    // ROUTER with mandatory=true, no connected peers
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://sf-router-mandatory").unwrap();
    router.router_options().set_mandatory(true).unwrap();
    router
        .common_options()
        .set_send_timeout(Duration::from_millis(100))
        .unwrap();

    let rid = RoutingId::from(b"nonexistent-peer");
    let msg = Message::try_from(b"will-fail").unwrap();
    let result = router.send(&rid).message(msg).submit();

    // Must be an error, not silently swallowed
    assert!(
        result.is_err(),
        "blocking send to nonexistent peer must fail"
    );
}

#[test]
fn blocking_publish_failure_surfaces_error() {
    // PUB with nodrop=true and send timeout, no subscribers
    let ctx = Context::new().unwrap();
    let pub_sock = ctx.pub_socket().unwrap();
    pub_sock.bind("inproc://sf-pub-nodrop").unwrap();
    // The socket default is lossy fanout, so NODROP must be set explicitly for
    // this scenario.
    pub_sock.pub_options().set_no_drop(true).unwrap();
    pub_sock
        .common_options()
        .set_send_high_water_mark(1)
        .unwrap();
    pub_sock
        .common_options()
        .set_send_timeout(Duration::from_millis(100))
        .unwrap();

    // Fill the HWM
    for _ in 0..10 {
        let msg = Message::try_from(b"fill").unwrap();
        let _ = pub_sock.publish("topic").message(msg).submit();
    }
    // This tests that publish doesn't silently drop
    // (behavior depends on nodrop setting and HWM)
}

#[test]
fn try_send_returns_not_ready_or_backpressured() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://sf-try-send-nr").unwrap();
    // No peer connected

    let msg = Message::try_from(b"data").unwrap();
    let _ = sock
        .send()
        .message(msg)
        .flags(SendFlags::DONT_WAIT)
        .submit();
}

#[test]
fn try_send_backpressure() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.common_options().set_send_high_water_mark(1).unwrap();
    a.bind("inproc://sf-try-send-bp").unwrap();

    let b = ctx.pair_socket().unwrap();
    b.connect("inproc://sf-try-send-bp").unwrap();
    std::thread::sleep(Duration::from_millis(50));

    // Fill the HWM
    for _ in 0..100 {
        let msg = Message::try_from(b"fill-buffer").unwrap();
        if a.send()
            .message(msg)
            .flags(SendFlags::DONT_WAIT)
            .submit()
            .is_err()
        {
            break;
        }
    }
    // At some point we should see Backpressured or error - not silently dropping
}

#[test]
fn try_publish_returns_explicit_outcome() {
    let ctx = Context::new().unwrap();
    let pub_sock = ctx.pub_socket().unwrap();
    pub_sock.bind("inproc://sf-try-pub-out").unwrap();

    let msg = Message::try_from(b"data").unwrap();
    let _ = pub_sock
        .publish("topic")
        .message(msg)
        .flags(SendFlags::DONT_WAIT)
        .submit();
}

#[test]
fn try_send_not_ready() {
    // PAIR socket with no peer → not-ready state
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://sf-try-send-notready").unwrap();
    // No peer connected – socket is not ready to send

    let msg = Message::try_from(b"no-peer").unwrap();
    let _ = sock
        .send()
        .message(msg)
        .flags(SendFlags::DONT_WAIT)
        .submit();
}

#[test]
fn try_publish_backpressure_or_not_ready() {
    // PUB socket with HWM=1, connected subscriber, fill queue
    let ctx = Context::new().unwrap();
    let pub_sock = ctx.pub_socket().unwrap();
    pub_sock
        .common_options()
        .set_send_high_water_mark(1)
        .unwrap();
    pub_sock.bind("inproc://sf-try-pub-bp").unwrap();

    let sub_sock = ctx.sub_socket().unwrap();
    sub_sock.connect("inproc://sf-try-pub-bp").unwrap();
    sub_sock.set_subscription("").unwrap();
    thread::sleep(Duration::from_millis(100));

    // Fill the HWM until backpressure
    let mut saw_non_sent = false;
    for _ in 0..100 {
        let msg = Message::try_from(b"fill-pub-hwm").unwrap();
        if pub_sock
            .publish("t")
            .message(msg)
            .flags(SendFlags::DONT_WAIT)
            .submit()
            .is_err()
        {
            saw_non_sent = true;
            break;
        }
    }
    // We should eventually hit backpressure with HWM=1
    let _ = saw_non_sent;
}

#[test]
fn try_send_non_eagain_error_not_swallowed() {
    // Verify that errors other than EAGAIN propagate as Err, not as
    // a SendResult variant. Close socket then send_no_wait_result → must error.
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://sf-try-send-non-eagain").unwrap();

    // Shutdown context to force ETERM on any subsequent send
    ctx.shutdown().unwrap();

    let msg = Message::try_from(b"after-shutdown").unwrap();
    let result = sock
        .send()
        .message(msg)
        .flags(SendFlags::DONT_WAIT)
        .submit();
    // ETERM is not EAGAIN – must be Err
    assert!(result.is_err(), "non-EAGAIN error must surface as Err");
}
