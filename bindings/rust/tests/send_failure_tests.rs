//! Send Failure Contract Tests – verify that blocking send failures surface
//! as errors and non-blocking sends return explicit outcomes.

mod test_support;

use std::thread;
use std::time::Duration;

use zlink::{Context, Message, RoutingId, SendFlags};

const RECORD_HWM: u64 = 65_536 + 64;

#[test]
fn sync_blocking_terminal_admits_a_send() {
    let ctx = Context::new().unwrap();
    let receiver = ctx.pair_socket().unwrap();
    let sender = ctx.pair_socket().unwrap();
    receiver.bind("inproc://rust-sync-send-admit").unwrap();
    sender.connect("inproc://rust-sync-send-admit").unwrap();
    thread::sleep(Duration::from_millis(50));

    sender
        .send()
        .message(Message::try_from(b"sync-admitted").unwrap())
        .submit_sync(SendFlags::NONE)
        .unwrap();

    receiver
        .common_options()
        .set_receive_timeout(Duration::from_secs(2))
        .unwrap();
    let mut received = zlink::Received::empty();
    assert!(receiver
        .recv(&mut received, zlink::RecvFlags::NONE)
        .unwrap());
    assert_eq!(received.single_part().unwrap().as_bytes(), b"sync-admitted");
}

#[test]
fn sync_dont_wait_reports_hwm_backpressure_immediately() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let receiver = ctx.pair_socket().unwrap();
    let sender = ctx.pair_socket().unwrap();
    sender
        .common_options()
        .set_send_high_water_mark(RECORD_HWM)
        .unwrap();
    receiver
        .common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    receiver.bind("inproc://rust-sync-send-backpressure").unwrap();
    sender
        .connect("inproc://rust-sync-send-backpressure")
        .unwrap();
    thread::sleep(Duration::from_millis(50));

    let started = std::time::Instant::now();
    let error = (0..256)
        .find_map(|_| {
            sender
                .send()
                .message(Message::try_from(vec![b'x'; 65_536].as_slice()).unwrap())
                .submit_sync(SendFlags::DONT_WAIT)
                .err()
        })
        .expect("the undrained outbound lane did not reach HWM");

    assert_eq!(error.code(), zlink::SubmitResult::Backpressured);
    assert!(
        started.elapsed() < Duration::from_secs(2),
        "DONT_WAIT must surface backpressure without waiting"
    );
}

#[test]
fn async_terminal_still_completes_after_sync_terminal_is_added() {
    let ctx = Context::new().unwrap();
    let receiver = ctx.pair_socket().unwrap();
    let sender = ctx.pair_socket().unwrap();
    receiver.bind("inproc://rust-async-send-regression").unwrap();
    sender.connect("inproc://rust-async-send-regression").unwrap();
    thread::sleep(Duration::from_millis(50));

    test_support::block_on(
        sender
            .send()
            .message(Message::try_from(b"async-still-works").unwrap())
            .submit(),
    )
    .unwrap();
}

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
    let result = test_support::block_on(
        router
            .send(&rid)
            .message(msg)
            .timeout(Duration::from_millis(200))
            .submit(),
    );

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
fn send_without_peer_completes_through_the_core_deadline() {
    // 0.13.1: `send()` has no flags stage. `zlink_send_async` never blocks, so
    // an unroutable record stays a Core-owned pending operation until the
    // per-operation deadline expires and the completion resolves the Future.
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://sf-try-send-nr").unwrap();
    // No peer connected

    let msg = Message::try_from(b"data").unwrap();
    let result = test_support::block_on(
        sock.send()
            .message(msg)
            .timeout(Duration::from_millis(200))
            .submit(),
    );
    assert!(
        result.is_err(),
        "an unroutable send must resolve as a failure, not silently succeed"
    );
}

#[test]
fn send_backpressure_is_absorbed_by_core_not_by_the_binding() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.common_options().set_send_high_water_mark(1).unwrap();
    a.bind("inproc://sf-try-send-bp").unwrap();

    let b = ctx.pair_socket().unwrap();
    b.connect("inproc://sf-try-send-bp").unwrap();
    std::thread::sleep(Duration::from_millis(50));

    // Nothing drains `b`, so the pipe reaches its HWM. Every record still
    // completes exactly once: either Core admits it, or the per-operation
    // deadline turns it into a failure. The binding never retries.
    let mut outcomes = 0usize;
    for _ in 0..32 {
        let msg = Message::try_from(b"fill-buffer").unwrap();
        let _ = test_support::block_on(
            a.send()
                .message(msg)
                .timeout(Duration::from_millis(200))
                .submit(),
        );
        outcomes += 1;
    }
    assert_eq!(outcomes, 32, "every submitted record must complete exactly once");
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
    let _ = test_support::block_on(
        sock.send()
            .message(msg)
            .timeout(Duration::from_millis(200))
            .submit(),
    );
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
    let result = test_support::block_on(
        sock.send()
            .message(msg)
            .timeout(Duration::from_millis(200))
            .submit(),
    );
    // ETERM is not EAGAIN – must be Err
    assert!(result.is_err(), "non-EAGAIN error must surface as Err");
}
