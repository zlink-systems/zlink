//! Exact-target asynchronous HWM admission contracts.

mod test_support;

use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use zlink::{
    Context, Message, Received, RecvFlags, RequestResult, RoutingId, SubmitResult, ZlinkError,
};

const RECORD_HWM: u64 = 65_536 + 64;

fn large_filler(byte: u8) -> Message {
    Message::try_from(vec![byte; 65_536].as_slice()).expect("filler message")
}

#[test]
fn dealer_multipart_submit_waits_without_occupying_the_caller() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    dealer
        .common_options()
        .set_send_high_water_mark(RECORD_HWM)
        .unwrap();
    router
        .common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    router.bind("inproc://rust-routed-async-multipart").unwrap();
    dealer
        .connect("inproc://rust-routed-async-multipart")
        .unwrap();
    thread::sleep(Duration::from_millis(75));

    dealer
        .common_options()
        .set_send_timeout(Duration::ZERO)
        .unwrap();
    let mut saturated = false;
    for _ in 0..16 {
        let result = test_support::block_on(dealer.send().message(large_filler(b'f')).submit());
        if result
            .as_ref()
            .is_err_and(|error| error.code() == SubmitResult::Backpressured)
        {
            saturated = true;
            break;
        }
        result.unwrap();
    }
    assert!(saturated, "test target did not reach HWM");

    dealer
        .common_options()
        .set_send_timeout(Duration::from_secs(3))
        .unwrap();
    let pending = dealer
        .send()
        .message(Message::try_from(b"pending-header").unwrap())
        .message(Message::try_from(b"pending-body").unwrap())
        .submit();
    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(pending)).unwrap();
    });
    assert!(done_rx.recv_timeout(Duration::from_millis(50)).is_err());

    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(2))
        .unwrap();
    let mut received = Received::empty();
    loop {
        assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
        if received.parts()[0].as_bytes() == b"pending-header" {
            break;
        }
    }
    done_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("pending send did not resume")
        .unwrap();
    waiter.join().unwrap();
    assert_eq!(received.parts().len(), 2);
    assert_eq!(received.parts()[1].as_bytes(), b"pending-body");
}

#[test]
fn blocked_router_target_does_not_delay_another_target() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer_a = ctx.dealer_socket().unwrap();
    let dealer_b = ctx.dealer_socket().unwrap();
    let rid_a = RoutingId::from(b"rust-async-target-a");
    let rid_b = RoutingId::from(b"rust-async-target-b");
    dealer_a.set_routing_id(&rid_a).unwrap();
    dealer_b.set_routing_id(&rid_b).unwrap();
    router
        .common_options()
        .set_send_high_water_mark(RECORD_HWM)
        .unwrap();
    dealer_a
        .common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    dealer_b
        .common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    router.bind("inproc://rust-routed-async-targets").unwrap();
    dealer_a
        .connect("inproc://rust-routed-async-targets")
        .unwrap();
    dealer_b
        .connect("inproc://rust-routed-async-targets")
        .unwrap();
    thread::sleep(Duration::from_millis(100));

    router
        .common_options()
        .set_send_timeout(Duration::ZERO)
        .unwrap();
    let mut saturated = false;
    for _ in 0..16 {
        let result =
            test_support::block_on(router.send(&rid_a).message(large_filler(b'a')).submit());
        if result
            .as_ref()
            .is_err_and(|error| error.code() == SubmitResult::Backpressured)
        {
            saturated = true;
            break;
        }
        result.unwrap();
    }
    assert!(saturated, "target A did not reach HWM");

    router
        .common_options()
        .set_send_timeout(Duration::from_secs(3))
        .unwrap();
    let blocked = router
        .send(&rid_a)
        .message(Message::try_from(b"blocked-a").unwrap())
        .submit();
    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(blocked)).unwrap();
    });
    assert!(done_rx.recv_timeout(Duration::from_millis(50)).is_err());

    test_support::block_on(
        router
            .send(&rid_b)
            .message(Message::try_from(b"ready-b").unwrap())
            .submit(),
    )
    .unwrap();
    let mut received_b = Received::empty();
    assert!(dealer_b.recv(&mut received_b, RecvFlags::NONE).unwrap());
    assert_eq!(received_b.parts()[0].as_bytes(), b"ready-b");
    assert!(done_rx.recv_timeout(Duration::from_millis(50)).is_err());

    let mut received_a = Received::empty();
    loop {
        assert!(dealer_a.recv(&mut received_a, RecvFlags::NONE).unwrap());
        if received_a.parts()[0].as_bytes() == b"blocked-a" {
            break;
        }
    }
    done_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("target A did not resume")
        .unwrap();
    waiter.join().unwrap();
}

#[test]
fn closing_a_socket_completes_its_pending_send_once() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let router = ctx.router_socket().unwrap();
    let mut dealer = ctx.dealer_socket().unwrap();
    dealer
        .common_options()
        .set_send_high_water_mark(RECORD_HWM)
        .unwrap();
    router
        .common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    router.bind("inproc://rust-routed-async-close").unwrap();
    dealer.connect("inproc://rust-routed-async-close").unwrap();
    thread::sleep(Duration::from_millis(75));

    dealer
        .common_options()
        .set_send_timeout(Duration::ZERO)
        .unwrap();
    let mut saturated = false;
    for _ in 0..16 {
        let result = test_support::block_on(dealer.send().message(large_filler(b'c')).submit());
        if result
            .as_ref()
            .is_err_and(|error| error.code() == SubmitResult::Backpressured)
        {
            saturated = true;
            break;
        }
        result.unwrap();
    }
    assert!(saturated, "test target did not reach HWM");

    dealer
        .common_options()
        .set_send_timeout(Duration::from_secs(3))
        .unwrap();
    let pending = dealer
        .send()
        .message(Message::try_from(b"pending-close").unwrap())
        .submit();
    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(pending)).unwrap();
    });
    assert!(done_rx.recv_timeout(Duration::from_millis(50)).is_err());

    dealer.close().unwrap();
    let error = done_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("close did not complete pending send")
        .unwrap_err();
    assert_eq!(error.code(), SubmitResult::Terminated);
    waiter.join().unwrap();
}

#[test]
fn request_deadline_is_not_extended_before_first_poll() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router
        .bind("inproc://rust-routed-async-request-deadline")
        .unwrap();
    dealer
        .connect("inproc://rust-routed-async-request-deadline")
        .unwrap();
    thread::sleep(Duration::from_millis(75));

    let pending = dealer
        .request()
        .message(Message::try_from(b"expired-before-poll").unwrap())
        .timeout(Duration::from_millis(10))
        .submit();
    thread::sleep(Duration::from_millis(30));

    let started = std::time::Instant::now();
    let error = match test_support::block_on(pending) {
        Ok(_) => panic!("expired request unexpectedly submitted"),
        Err(error) => error,
    };
    assert!(started.elapsed() < Duration::from_millis(100));
    assert!(matches!(
        error,
        ZlinkError::Request(request) if request.code() == RequestResult::TimedOut
    ));

    let mut received = Received::empty();
    assert!(!router.recv(&mut received, RecvFlags::DONT_WAIT).unwrap());
}
