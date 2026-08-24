//! Core-driven send-completion contracts for the asynchronous send terminals.
//!
//! `submit()` returns a runtime-independent `Future` whose completion is driven by
//! `zlink_send_complete_handler`. The binding owns no thread, no park queue, no
//! retry and no deadline timer; Core answers every operation exactly once.

mod test_support;

use std::sync::mpsc;
use std::task::Poll;
use std::thread;
use std::time::Duration;

use zlink::{
    Context, Message, Received, RecvFlags, RequestResult, RoutingId, SubmitResult, ZlinkError,
};

const RECORD_HWM: u64 = 65_536 + 64;

fn large_filler(byte: u8) -> Message {
    Message::try_from(vec![byte; 65_536].as_slice()).expect("filler message")
}

/// Fills the outbound lane until at least one record is left as a Core-owned
/// pending operation, and returns those still-pending futures.
type PendingSend = std::pin::Pin<Box<dyn std::future::Future<Output = Result<(), zlink::SubmitError>> + Send>>;

fn saturate(mut submit: impl FnMut() -> PendingSend) -> Vec<PendingSend> {
    let mut pending = Vec::new();
    for _ in 0..24 {
        let mut future = submit();
        match test_support::poll_once(&mut future) {
            Poll::Ready(Ok(())) => {}
            // Core's own pending bound is a terminal answer, not a retry hint:
            // the application owns that policy.
            Poll::Ready(Err(_)) => break,
            Poll::Pending => pending.push(future),
        }
    }
    pending
}

#[test]
fn inline_admission_resolves_the_future_on_its_first_poll() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router.bind("inproc://rust-send-complete-inline").unwrap();
    dealer.connect("inproc://rust-send-complete-inline").unwrap();
    thread::sleep(Duration::from_millis(75));

    let mut future = Box::pin(
        dealer
            .send()
            .message(Message::try_from(b"inline-header").unwrap())
            .message(Message::try_from(b"inline-body").unwrap())
            .submit(),
    );
    assert_eq!(
        test_support::poll_once(&mut future),
        Poll::Ready(Ok(())),
        "an immediately admitted record must complete inside the first poll"
    );

    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(2))
        .unwrap();
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts().len(), 2);
    assert_eq!(received.parts()[0].as_bytes(), b"inline-header");
    assert_eq!(received.parts()[1].as_bytes(), b"inline-body");
}

#[test]
fn backpressured_send_completes_when_the_reader_drains() {
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
    router.bind("inproc://rust-send-complete-drain").unwrap();
    dealer.connect("inproc://rust-send-complete-drain").unwrap();
    thread::sleep(Duration::from_millis(75));

    let pending = saturate(|| Box::pin(dealer.send().message(large_filler(b'f')).submit()));
    assert!(
        !pending.is_empty(),
        "no record reached the Core pending lane"
    );

    // Nothing in the binding retries: draining the reader is what lets Core
    // admit the parked records and run their completions. The drain has to run
    // while the futures are awaited, so it gets its own thread.
    let (stop_tx, stop_rx) = mpsc::channel::<()>();
    let reader = thread::spawn(move || {
        let mut received = Received::empty();
        while stop_rx.try_recv().is_err() {
            match router.recv(&mut received, RecvFlags::DONT_WAIT) {
                Ok(true) => {}
                _ => thread::sleep(Duration::from_millis(5)),
            }
        }
    });

    for future in pending {
        test_support::block_on(future).expect("parked record never completed");
    }
    stop_tx.send(()).unwrap();
    reader.join().unwrap();
}

#[test]
fn per_operation_timeout_completes_a_parked_record() {
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
    router.bind("inproc://rust-send-complete-timeout").unwrap();
    dealer
        .connect("inproc://rust-send-complete-timeout")
        .unwrap();
    thread::sleep(Duration::from_millis(75));

    let pending = saturate(|| Box::pin(dealer.send().message(large_filler(b't')).submit()));
    assert!(!pending.is_empty(), "test target did not reach HWM");

    // The deadline is a per-operation Core option, not a binding timer.
    let started = std::time::Instant::now();
    let outcome = test_support::block_on(
        dealer
            .send()
            .message(large_filler(b'x'))
            .timeout(Duration::from_millis(250))
            .submit(),
    );
    assert!(
        outcome.is_err(),
        "a record that never gets admitted must fail at its deadline"
    );
    assert!(
        started.elapsed() < Duration::from_secs(5),
        "the deadline must be Core-owned and prompt"
    );
    drop(pending);
}

#[test]
fn dropping_a_pending_send_future_requests_cancellation() {
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
    router.bind("inproc://rust-send-complete-cancel").unwrap();
    dealer.connect("inproc://rust-send-complete-cancel").unwrap();
    thread::sleep(Duration::from_millis(75));

    let pending = saturate(|| Box::pin(dealer.send().message(large_filler(b'd')).submit()));
    assert!(!pending.is_empty(), "test target did not reach HWM");

    let mut cancelled = Box::pin(dealer.send().message(large_filler(b'z')).submit());
    assert_eq!(test_support::poll_once(&mut cancelled), Poll::Pending);
    // Drop requests `zlink_send_async_cancel`. The operation still completes
    // exactly once inside Core; the binding keeps the op state alive until it
    // does, so this must neither hang nor double-free.
    drop(cancelled);
    drop(pending);

    let mut received = Received::empty();
    let _ = router.recv(&mut received, RecvFlags::DONT_WAIT);
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

    let filler = saturate(|| Box::pin(dealer.send().message(large_filler(b'c')).submit()));
    assert!(!filler.is_empty(), "test target did not reach HWM");

    let mut pending = Box::pin(
        dealer
            .send()
            .message(Message::try_from(b"pending-close").unwrap())
            .submit(),
    );
    assert_eq!(test_support::poll_once(&mut pending), Poll::Pending);

    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(pending)).unwrap();
    });
    assert!(done_rx.recv_timeout(Duration::from_millis(50)).is_err());

    dealer.close().unwrap();
    let outcome = done_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("close did not complete the pending send");
    let error = outcome.expect_err("a closed socket cannot admit a parked record");
    // Core reports close as a terminal completion with ECANCELED; the binding
    // surfaces that as a not-admitted submit outcome.
    assert!(matches!(
        error.code(),
        SubmitResult::NotAdmitted | SubmitResult::Terminated
    ));
    waiter.join().unwrap();
    drop(filler);
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

    let blocked_a = saturate(|| Box::pin(router.send(&rid_a).message(large_filler(b'a')).submit()));
    assert!(!blocked_a.is_empty(), "target A did not reach HWM");

    // Target B is a different physical pipe, so its record is admitted right
    // away even while target A is parked.
    test_support::block_on(
        router
            .send(&rid_b)
            .message(Message::try_from(b"ready-b").unwrap())
            .submit(),
    )
    .unwrap();
    let mut received_b = Received::empty();
    dealer_b
        .common_options()
        .set_receive_timeout(Duration::from_secs(2))
        .unwrap();
    assert!(dealer_b.recv(&mut received_b, RecvFlags::NONE).unwrap());
    assert_eq!(received_b.parts()[0].as_bytes(), b"ready-b");

    let (stop_tx, stop_rx) = mpsc::channel::<()>();
    let reader = thread::spawn(move || {
        let mut received_a = Received::empty();
        while stop_rx.try_recv().is_err() {
            match dealer_a.recv(&mut received_a, RecvFlags::DONT_WAIT) {
                Ok(true) => {}
                _ => thread::sleep(Duration::from_millis(5)),
            }
        }
    });
    for future in blocked_a {
        test_support::block_on(future).expect("target A never resumed");
    }
    stop_tx.send(()).unwrap();
    reader.join().unwrap();
}

#[test]
fn request_timeout_is_owned_by_core() {
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

    // The Future is inert until polled; the submit and the Core-owned deadline
    // both start at the first poll. Nothing in the binding times the request.
    let pending = dealer
        .request()
        .message(Message::try_from(b"no-responder").unwrap())
        .timeout(Duration::from_millis(50))
        .submit();
    thread::sleep(Duration::from_millis(30));

    let started = std::time::Instant::now();
    let error = match test_support::block_on(pending) {
        Ok(_) => panic!("request unexpectedly answered without a responder"),
        Err(error) => error,
    };
    assert!(started.elapsed() < Duration::from_secs(2));
    assert!(matches!(
        error,
        ZlinkError::Request(request) if request.code() == RequestResult::TimedOut
    ));
}
