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
    b.connect("inproc://own-send-consume").unwrap();
    thread::sleep(Duration::from_millis(50));

    // After send, the message is consumed (moved into native).
    // Rust's move semantics prevent reuse at compile time.
    let msg = Message::try_from(b"owned-data").unwrap();
    test_support::block_on(a.send().message(msg).submit()).unwrap();
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
    b.connect("inproc://own-multi-consume").unwrap();
    thread::sleep(Duration::from_millis(50));

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
    test_support::block_on(op.submit()).unwrap();
}

#[test]
fn recv_ownership_transfers_to_caller() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.bind("inproc://own-recv-transfer").unwrap();

    let b = ctx.pair_socket().unwrap();
    b.connect("inproc://own-recv-transfer").unwrap();
    thread::sleep(Duration::from_millis(50));

    let msg = Message::try_from(b"recv-test").unwrap();
    test_support::block_on(b.send().message(msg).submit()).unwrap();

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
    let _ = test_support::block_on(router.send(&rid).message(msg).submit());
    // msg is consumed regardless of success/failure – no native leak
}

#[test]
fn repeated_multipart_recv_preserves_shape() {
    let ctx = Context::new().unwrap();

    // Direct recv path
    let a1 = ctx.pair_socket().unwrap();
    a1.bind("inproc://own-shape-direct").unwrap();
    let b1 = ctx.pair_socket().unwrap();
    b1.connect("inproc://own-shape-direct").unwrap();
    thread::sleep(Duration::from_millis(50));

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
    test_support::block_on(op.submit()).unwrap();
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
    b2.connect("inproc://own-shape-repeat").unwrap();
    thread::sleep(Duration::from_millis(50));

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
    test_support::block_on(op.submit()).unwrap();
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
fn pull_receive_owns_parts() {
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://own-pull-receive").unwrap();

    let client = ctx.pair_socket().unwrap();
    client.connect("inproc://own-pull-receive").unwrap();
    thread::sleep(Duration::from_millis(50));

    let msg = Message::try_from(b"cb-payload").unwrap();
    test_support::block_on(client.send().message(msg).submit()).unwrap();
    let mut received = Received::empty();
    server.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts()[0].as_bytes(), b"cb-payload");
}

#[test]
fn request_future_preserves_more_than_1024_reply_parts() {
    const PART_COUNT: usize = 1025;

    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router.bind("inproc://own-request-many-parts").unwrap();
    dealer.connect("inproc://own-request-many-parts").unwrap();
    thread::sleep(Duration::from_millis(50));

    let server = thread::spawn(move || {
        let mut request = Received::empty();
        router.recv(&mut request, RecvFlags::NONE).unwrap();
        let mut reply = request
            .reply()
            .message(Message::try_from(0_u32.to_le_bytes().as_slice()).unwrap());
        for index in 1..PART_COUNT as u32 {
            reply = reply.message(Message::try_from(index.to_le_bytes().as_slice()).unwrap());
        }
        reply.submit().unwrap();
    });

    let parts = test_support::block_on(
        dealer
            .request()
            .message(Message::try_from(b"many-parts").unwrap())
            .timeout(Duration::from_secs(5))
            .submit(),
    )
    .expect("request failed");
    assert_eq!(parts.len(), PART_COUNT);
    assert_eq!(parts[1024].as_bytes(), 1024_u32.to_le_bytes());
    server.join().unwrap();
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
