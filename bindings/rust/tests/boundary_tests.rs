//! Boundary Validation Tests – verify fail-fast behavior for
//! routing-id length, duration overflow, null checks, etc.

use std::sync::{Arc, Barrier};
use std::thread;
use std::time::Duration;

use zlink::{Context, Message, RoutingId};

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn routing_id_max_length_accepted() {
    let data = vec![0x42u8; 255];
    let rid = RoutingId::from(data.as_slice());
    assert_eq!(rid.size(), 255, "255-byte routing id must succeed");
}

#[test]
fn routing_id_exceeds_max_fails() {
    let data = vec![0x42u8; 256];
    let result = std::panic::catch_unwind(|| RoutingId::from(data.as_slice()));
    assert!(result.is_err(), "256-byte routing id must fail immediately");
}

#[test]
fn routing_id_empty_fails() {
    let result = std::panic::catch_unwind(|| RoutingId::from(&[] as &[u8]));
    assert!(result.is_err(), "empty routing id must fail");
}

#[test]
fn routing_id_one_byte_accepted() {
    let rid = RoutingId::from(&[0x01]);
    assert_eq!(rid.size(), 1);
}

#[test]
fn routing_id_hex_and_display_policy() {
    let rid = RoutingId::from(&[0x00, 0x41, 0x42]);
    assert_eq!(RoutingId::from_hex("004142"), rid);
    assert_eq!(RoutingId::try_from_hex("004142").unwrap(), rid);
    assert_eq!(rid.to_string(), "hex:004142");
    assert_eq!(
        RoutingId::try_from_hex(&"a".repeat(510)).unwrap().size(),
        255
    );
    let result = std::panic::catch_unwind(|| RoutingId::from_hex("not-hex"));
    assert!(result.is_err(), "invalid hex routing id string must fail");
    let oversized = "a".repeat(512);
    let result = std::panic::catch_unwind(|| RoutingId::from_hex(&oversized));
    assert!(result.is_err(), "oversized hex routing id string must fail");
    assert!(
        RoutingId::try_from_hex(&oversized).is_err(),
        "oversized hex routing id string must return ConfigError"
    );
    assert_eq!(RoutingId::from("dealer-1").to_string(), "dealer-1");
    assert_eq!(RoutingId::from(23u32).to_string(), "23");
}

#[test]
fn duration_overflow_fails() {
    // Duration larger than i32::MAX milliseconds
    let huge = Duration::from_millis(i32::MAX as u64 + 1);
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let result = sock.common_options().set_linger(huge);
    assert!(result.is_err(), "duration overflow must be rejected");
}

#[test]
fn duration_max_accepted() {
    let max_ok = Duration::from_millis(i32::MAX as u64);
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let result = sock.common_options().set_linger(max_ok);
    assert!(result.is_ok(), "i32::MAX ms duration must be accepted");
}

#[test]
fn context_is_send_sync_and_shared_socket_creation_is_safe() {
    assert_send_sync::<Context>();

    let ctx = Arc::new(Context::new().unwrap());
    let barrier = Arc::new(Barrier::new(5));
    let mut workers = Vec::new();

    for index in 0..4 {
        let ctx = Arc::clone(&ctx);
        let barrier = Arc::clone(&barrier);
        workers.push(thread::spawn(move || {
            barrier.wait();
            let sock = ctx.pair_socket().unwrap();
            sock.common_options()
                .set_linger(Duration::from_millis(0))
                .unwrap();
            let endpoint = format!("inproc://rust-context-thread-safety-{index}");
            sock.bind(&endpoint).unwrap();
        }));
    }

    barrier.wait();

    for worker in workers {
        worker.join().unwrap();
    }
}

#[test]
fn null_byte_in_address_rejected() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let result = sock.bind("inproc://bad\0addr");
    assert!(result.is_err(), "address with null byte must be rejected");
}

#[test]
fn null_byte_in_topic_rejected() {
    let ctx = Context::new().unwrap();
    let pub_sock = ctx.pub_socket().unwrap();
    pub_sock.bind("inproc://bnd-topic-null").unwrap();

    let result = std::panic::catch_unwind(|| pub_sock.publish("bad\0topic"));
    assert!(result.is_err(), "topic with null byte must be rejected");
}

#[test]
fn null_byte_in_subscription_filter_rejected() {
    let ctx = Context::new().unwrap();
    let sub = ctx.sub_socket().unwrap();
    sub.bind("inproc://bnd-filter-null-target").unwrap();

    let result = sub.set_subscription("bad\0filter");
    assert!(result.is_err(), "filter with null byte must be rejected");
}

#[test]
fn message_try_from_bytes() {
    let msg = Message::try_from(&b"hello"[..]);
    assert!(msg.is_ok());
    assert_eq!(msg.unwrap().as_bytes(), b"hello");
}

#[test]
fn message_try_from_str() {
    let msg = Message::try_from(b"world");
    assert!(msg.is_ok());
    assert_eq!(msg.unwrap().as_str().unwrap(), "world");
}
