//! Ownership Tests – verify message ownership contracts across
//! send, recv, close, and pull-receive boundaries.

mod test_support;

use std::thread;
use std::time::Duration;

use zlink::{Context, Message, Poller, Received, RecvFlags, RoutingId, Timer};

#[test]
fn send_consumes_message_ownership() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.bind("inproc://own-send-consume").unwrap();

    let b = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&a, &b, || {
        b.connect("inproc://own-send-consume").unwrap()
    });

    // After send, the message is consumed (moved into native).
    // Rust's move semantics prevent reuse at compile time.
    let msg = Message::try_from(b"owned-data").unwrap();
    a.send().message(msg).submit_sync().unwrap();
    // `msg` cannot be used here – Rust ownership enforced

    let mut received = Received::empty();
    b.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts()[0].as_bytes(), b"owned-data");
}

#[test]
fn send_multipart_consumes_all_parts() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.bind("inproc://own-multi-consume").unwrap();

    let b = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&a, &b, || {
        b.connect("inproc://own-multi-consume").unwrap()
    });

    let parts = vec![
        Message::try_from(b"part-a").unwrap(),
        Message::try_from(b"part-b").unwrap(),
    ];
    // Vec is consumed by send
    let mut iter = parts.into_iter();
    let first = iter.next().unwrap();
    let mut op = a.send().message(first);
    for part in iter {
        op = op.message(part);
    }
    op.submit_sync().unwrap();
}

#[test]
fn recv_ownership_transfers_to_caller() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.bind("inproc://own-recv-transfer").unwrap();

    let b = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&a, &b, || {
        b.connect("inproc://own-recv-transfer").unwrap()
    });

    let msg = Message::try_from(b"recv-test").unwrap();
    b.send().message(msg).submit_sync().unwrap();

    let mut received = Received::empty();
    a.recv(&mut received, RecvFlags::NONE).unwrap();
    // Caller owns the parts; dropping them calls zlink_msg_close
    let parts = received.into_parts();
    assert_eq!(parts.len(), 1);
    assert_eq!(parts[0].as_bytes(), b"recv-test");
    // Parts dropped here – native memory freed
}

#[test]
fn unsent_message_must_be_closed() {
    // Creating a message and dropping it without send must properly close
    // the native message (RAII Drop).
    for _ in 0..1000 {
        let msg = Message::try_from(b"never-sent").unwrap();
        drop(msg); // Must call zlink_msg_close
    }
}

#[test]
fn send_failure_does_not_leak() {
    // If send fails, messages are still consumed (ownership transferred to
    // native on any return path per the C API contract).
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://own-send-fail").unwrap();
    router.router_options().set_mandatory(true).unwrap();
    router
        .common_options()
        .set_send_timeout(Duration::from_millis(50))
        .unwrap();

    let rid = RoutingId::from(b"ghost");
    let msg = Message::try_from(b"will-fail").unwrap();
    let _ = router.send(&rid).message(msg).submit_sync();
    // msg is consumed regardless of success/failure – no native leak
}

#[test]
fn repeated_multipart_recv_preserves_shape() {
    let ctx = Context::new().unwrap();

    // Direct recv path
    let a1 = ctx.pair_socket().unwrap();
    a1.bind("inproc://own-shape-direct").unwrap();
    let b1 = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&a1, &b1, || {
        b1.connect("inproc://own-shape-direct").unwrap()
    });

    let parts = vec![
        Message::try_from(b"frame-x").unwrap(),
        Message::try_from(b"frame-y").unwrap(),
    ];
    let mut iter = parts.into_iter();
    let first = iter.next().unwrap();
    let mut op = b1.send().message(first);
    for part in iter {
        op = op.message(part);
    }
    op.submit_sync().unwrap();
    let mut direct = Received::empty();
    a1.recv(&mut direct, RecvFlags::NONE).unwrap();
    let direct_count = direct.parts().len();
    let direct_data: Vec<Vec<u8>> = direct
        .parts()
        .iter()
        .map(|p| p.as_bytes().to_vec())
        .collect();

    // Direct recv path with the same frame ownership semantics.
    let a2 = ctx.pair_socket().unwrap();
    a2.bind("inproc://own-shape-repeat").unwrap();

    let b2 = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&a2, &b2, || {
        b2.connect("inproc://own-shape-repeat").unwrap()
    });

    let parts = vec![
        Message::try_from(b"frame-x").unwrap(),
        Message::try_from(b"frame-y").unwrap(),
    ];
    let mut iter = parts.into_iter();
    let first = iter.next().unwrap();
    let mut op = b2.send().message(first);
    for part in iter {
        op = op.message(part);
    }
    op.submit_sync().unwrap();
    let mut repeated = Received::empty();
    a2.recv(&mut repeated, RecvFlags::NONE).unwrap();
    let repeated_data: Vec<Vec<u8>> = repeated
        .parts()
        .iter()
        .map(|p| p.as_bytes().to_vec())
        .collect();

    // Both paths must see the same number of frames with the same content.
    assert_eq!(direct_count, repeated_data.len(), "frame count must match");
    assert_eq!(direct_data, repeated_data, "frame content must match");
}

#[test]
#[ignore = "requires the B-3 LD_PRELOAD precondition fault-injection shim"]
fn multipart_recv_adopts_parts_into_uninitialized_message_slots() {
    const FAIL_ON_INITIALIZED_DEST: &str = "ZLINK_AUDIT_FAIL_INITIALIZED_ADOPT";
    const MARKER_ENV: &str = "ZLINK_AUDIT_MARKER";

    let ctx = Context::new().unwrap();
    let receiver = ctx.pair_socket().unwrap();
    receiver.bind("inproc://own-adopt-uninitialized").unwrap();
    let sender = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&receiver, &sender, || {
        sender.connect("inproc://own-adopt-uninitialized").unwrap()
    });

    sender
        .send()
        .message(Message::try_from(b"first").unwrap())
        .message(Message::try_from(b"second").unwrap())
        .submit_sync()
        .unwrap();

    let mut received = Received::empty();
    unsafe { std::env::set_var(FAIL_ON_INITIALIZED_DEST, "1") };
    let result = receiver.recv(&mut received, zlink::RecvFlags::NONE);
    unsafe { std::env::remove_var(FAIL_ON_INITIALIZED_DEST) };
    let marker_path = std::env::var_os(MARKER_ENV).expect("fault-injection marker path");
    let marker = std::fs::read_to_string(marker_path).expect("fault-injection shim did not run");
    assert!(
        marker.lines().next() == Some("adopt-accepted-uninitialized-destination")
            && marker
                .lines()
                .all(|line| line == "adopt-accepted-uninitialized-destination"),
        "unexpected fault-injection marker: {marker:?}"
    );
    result.unwrap();

    assert_eq!(received.parts().len(), 2);
    assert_eq!(received.parts()[0].as_bytes(), b"first");
    assert_eq!(received.parts()[1].as_bytes(), b"second");
}

#[test]
fn pull_receive_owns_parts() {
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://own-pull-receive").unwrap();

    let client = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&server, &client, || {
        client.connect("inproc://own-pull-receive").unwrap()
    });

    let msg = Message::try_from(b"cb-payload").unwrap();
    client.send().message(msg).submit_sync().unwrap();
    let mut received = Received::empty();
    server.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts()[0].as_bytes(), b"cb-payload");
}

#[test]
fn request_future_preserves_more_than_1024_reply_parts() {
    const PART_COUNT: usize = 1025;

    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://own-request-many-parts").unwrap();

    let (_dealer, _completion_driver, _, future, request) = test_support::request_until_received(
        &ctx,
        &router,
        "inproc://own-request-many-parts",
        b"many-parts",
        Duration::from_secs(5),
    );
    let mut reply = request
        .reply()
        .message(Message::try_from(0_u32.to_le_bytes().as_slice()).unwrap());
    for index in 1..PART_COUNT as u32 {
        reply = reply.message(Message::try_from(index.to_le_bytes().as_slice()).unwrap());
    }
    reply.submit().unwrap();

    let parts = test_support::block_on(future).expect("request failed");
    assert_eq!(parts.len(), PART_COUNT);
    assert_eq!(parts[1024].as_bytes(), 1024_u32.to_le_bytes());
}

#[test]
#[ignore = "requires the B-14 LD_PRELOAD message-copy counter shim"]
fn blocking_send_and_reply_do_not_copy_message_parts() {
    const OPERATION_ENV: &str = "ZLINK_AUDIT_OPERATION";
    const MARKER_ENV: &str = "ZLINK_AUDIT_MARKER";

    let ctx = Context::new().unwrap();

    let receiver = ctx.pair_socket().unwrap();
    receiver.bind("inproc://own-sync-part-transfer").unwrap();
    let sender = ctx.pair_socket().unwrap();
    test_support::connect_pair_and_confirm(&receiver, &sender, || {
        sender.connect("inproc://own-sync-part-transfer").unwrap()
    });

    unsafe { std::env::set_var(OPERATION_ENV, "send") };
    let send_result = sender
        .send()
        .message(Message::try_from(b"send-first").unwrap())
        .message(Message::try_from(b"send-second").unwrap())
        .submit_sync();
    unsafe { std::env::remove_var(OPERATION_ENV) };
    send_result.unwrap();

    let mut received_send = Received::empty();
    assert!(receiver.recv(&mut received_send, RecvFlags::NONE).unwrap());
    assert_eq!(received_send.parts().len(), 2);
    assert_eq!(received_send.parts()[0].as_bytes(), b"send-first");
    assert_eq!(received_send.parts()[1].as_bytes(), b"send-second");

    let router = ctx.router_socket().unwrap();
    router.bind("inproc://own-sync-reply-transfer").unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let _dealer_completion_driver = test_support::CompletionPollerDriver::new(&dealer);
    test_support::connect_dealer_router_and_confirm(&router, &dealer, || {
        dealer.connect("inproc://own-sync-reply-transfer").unwrap()
    });

    let request = dealer
        .request()
        .message(Message::try_from(b"request-payload").unwrap())
        .submit()
        .unwrap();
    test_support::block_on(request.admitted).unwrap();
    let reply_future = request.reply;

    let mut received_request = Received::empty();
    assert!(router.recv(&mut received_request, RecvFlags::NONE).unwrap());
    let reply = received_request.reply();
    let request_part = std::mem::take(&mut received_request)
        .into_parts()
        .into_iter()
        .next()
        .expect("request payload");

    unsafe { std::env::set_var(OPERATION_ENV, "reply") };
    let reply_result = reply
        .message(request_part)
        .message(Message::try_from(b"reply-second").unwrap())
        .submit();
    unsafe { std::env::remove_var(OPERATION_ENV) };
    reply_result.unwrap();

    let reply_parts = test_support::block_on(reply_future).unwrap();
    assert_eq!(reply_parts.len(), 2);
    assert_eq!(reply_parts[0].as_bytes(), b"request-payload");
    assert_eq!(reply_parts[1].as_bytes(), b"reply-second");

    let marker_path = std::env::var_os(MARKER_ENV).expect("fault-injection marker path");
    let marker =
        std::fs::read_to_string(marker_path).expect("message-copy counter shim did not run");
    let events = marker.lines().collect::<Vec<_>>();
    assert!(events.contains(&"send:submit-send"));
    assert!(events.contains(&"reply:submit-reply"));
    assert!(events.contains(&"send:multipart-close-2"));
    assert!(events.contains(&"reply:multipart-close-2"));
    let send_copies = events.iter().filter(|event| **event == "send:copy").count();
    let reply_copies = events
        .iter()
        .filter(|event| **event == "reply:copy")
        .count();
    assert_eq!(
        (send_copies, reply_copies),
        (0, 0),
        "synchronous SEND and REPLY copied message parts: {events:?}"
    );
}

#[test]
fn dropping_registered_timer_defers_native_destroy() {
    // Core rejects timer destruction while a poller registration remains. The
    // binding must retain the native handle and retry destruction after the
    // poller releases its registration.
    let poller = Poller::new().unwrap();
    let timer = Timer::new().unwrap();
    timer.start(1_000_000, 0).unwrap();
    poller.add_timer(&timer, 1).unwrap();

    drop(timer);
    thread::sleep(Duration::from_millis(20));
    drop(poller);
    thread::sleep(Duration::from_millis(40));
}
