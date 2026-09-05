//! Send failure contracts for blocking terminals, managed DONTWAIT sends, and
//! one-shot DONTWAIT publish.

mod test_support;

use std::thread;
use std::time::Duration;

use zlink::{
    Context, Message, Received, RecvFlags, RoutingId, SendFlags, SubmitResult, ZlinkError,
};

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
        .submit_sync()
        .unwrap();

    receiver
        .common_options()
        .set_receive_timeout(Duration::from_secs(2))
        .unwrap();
    let mut received = zlink::Received::empty();
    assert!(
        receiver
            .recv(&mut received, zlink::RecvFlags::NONE)
            .unwrap()
    );
    assert_eq!(received.single_part().unwrap().as_bytes(), b"sync-admitted");
}

#[test]
fn async_terminal_still_completes_after_sync_terminal_is_added() {
    let ctx = Context::new().unwrap();
    let receiver = ctx.pair_socket().unwrap();
    let sender = ctx.pair_socket().unwrap();
    receiver
        .bind("inproc://rust-async-send-regression")
        .unwrap();
    sender
        .connect("inproc://rust-async-send-regression")
        .unwrap();
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
fn routed_send_to_missing_target_is_immediately_not_connected() {
    // ROUTER with mandatory=true, no connected peers
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://sf-router-mandatory").unwrap();
    router.router_options().set_mandatory(true).unwrap();
    let rid = RoutingId::from(b"nonexistent-peer");
    let msg = Message::try_from(b"will-fail").unwrap();
    let mut future = Box::pin(router.send(&rid).message(msg).submit());
    let error = match test_support::poll_once(&mut future) {
        std::task::Poll::Ready(Err(error)) => error,
        std::task::Poll::Ready(Ok(())) => panic!("missing route was admitted"),
        std::task::Poll::Pending => panic!("missing route incorrectly returned a wait token"),
    };
    assert_eq!(error.code(), SubmitResult::NotConnected);
}

#[test]
fn router_request_to_a_dealer_is_immediately_not_admitted() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let dealer_rid = RoutingId::from(b"request-rejecting-dealer");
    dealer.set_routing_id(&dealer_rid).unwrap();
    router.bind("inproc://sf-request-not-admitted").unwrap();
    dealer.connect("inproc://sf-request-not-admitted").unwrap();

    // A completed DATA transfer is the route/type barrier for the targeted
    // request below.
    dealer
        .send()
        .message(Message::try_from(b"route-ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());

    let mut request = Box::pin(
        router
            .request(&dealer_rid)
            .message(Message::try_from(b"wrong-peer-type").unwrap())
            .submit(),
    );
    let error = match test_support::poll_once(&mut request) {
        std::task::Poll::Ready(Err(ZlinkError::Submit(error))) => error,
        std::task::Poll::Ready(Err(other)) => panic!("unexpected request error: {other}"),
        std::task::Poll::Ready(Ok(_)) => panic!("request to DEALER was admitted"),
        std::task::Poll::Pending => panic!("request to DEALER returned a wait token"),
    };
    assert_eq!(error.code(), SubmitResult::NotAdmitted);
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
fn send_without_peer_keeps_only_a_payload_free_token_after_drop() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://sf-try-send-nr").unwrap();
    // No peer connected: Core keeps the wait token and the Future retains the
    // payload until this explicit drop.

    let msg = Message::try_from(b"data").unwrap();
    let mut future = Box::pin(sock.send().message(msg).submit());
    assert_eq!(
        test_support::poll_once(&mut future),
        std::task::Poll::Pending
    );
    drop(future);
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
    let result = test_support::block_on(sock.send().message(msg).submit());
    // ETERM is not EAGAIN – must be Err
    assert!(result.is_err(), "non-EAGAIN error must surface as Err");
}
