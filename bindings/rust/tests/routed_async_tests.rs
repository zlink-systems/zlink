//! WRITABLE retry contracts for asynchronous SEND and Core completion
//! contracts for REQUEST.
//!
//! `submit()` retains the logical SEND or REQUEST packet across DONTWAIT
//! backpressure and retries it only after the matching WRITABLE token is
//! pulled. After REQUEST admission, reply and deadline completion remain
//! Core-owned.

mod test_support;

use std::sync::mpsc;
use std::task::Poll;
use std::thread;
use std::time::Duration;

use zlink::{
    Context, Message, POLLCOMPLETION, POLLOUT, PollEvent, Poller, Received, RecvFlags,
    RequestResult, RoutingId, SubmitResult, ZlinkError,
};

const RECORD_HWM: u64 = 65_536 + 64;

fn large_filler(byte: u8) -> Message {
    Message::try_from(vec![byte; 65_536].as_slice()).expect("filler message")
}

/// Fills the outbound lane until at least one binding-owned SEND is waiting on
/// a Core WRITABLE token, and returns those still-pending futures.
type PendingSend =
    std::pin::Pin<Box<dyn std::future::Future<Output = Result<(), zlink::SubmitError>> + Send>>;

fn saturate(mut submit: impl FnMut() -> PendingSend) -> Vec<PendingSend> {
    let mut pending = Vec::new();
    for _ in 0..24 {
        let mut future = submit();
        match test_support::poll_once(&mut future) {
            Poll::Ready(Ok(())) => {}
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
    dealer
        .connect("inproc://rust-send-complete-inline")
        .unwrap();
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
fn public_poller_drains_writable_and_retries_the_same_packet() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let receiver = ctx.pair_socket().unwrap();
    let sender = ctx.pair_socket().unwrap();
    sender
        .common_options()
        .set_send_high_water_mark(512)
        .unwrap();
    receiver
        .common_options()
        .set_receive_high_water_mark(512)
        .unwrap();
    sender.common_options().set_immediate(true).unwrap();
    sender
        .common_options()
        .set_send_timeout(Duration::from_secs(5))
        .unwrap();
    receiver
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    receiver
        .bind("inproc://rust-writable-public-poller")
        .unwrap();
    sender
        .connect("inproc://rust-writable-public-poller")
        .unwrap();

    // A blocking round trip is the connection barrier; this test uses no
    // sleep, helper thread, or timer.
    sender
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut handshake = Received::empty();
    assert!(receiver.recv(&mut handshake, RecvFlags::NONE).unwrap());
    assert_eq!(handshake.single_part().unwrap().as_bytes(), b"ready");

    let mut admitted = Vec::new();
    let mut waiting = None;
    for index in 0..512usize {
        let header = format!("{index:04}").into_bytes();
        let mut body = vec![b'x'; 64];
        body[..header.len()].copy_from_slice(&header);
        let mut future = Box::pin(
            sender
                .send()
                .message(Message::try_from(header.as_slice()).unwrap())
                .message(Message::try_from(body.as_slice()).unwrap())
                .submit(),
        );
        match test_support::poll_once(&mut future) {
            Poll::Ready(Ok(())) => admitted.push((header, body)),
            Poll::Ready(Err(error)) => panic!("unexpected SEND failure: {error}"),
            Poll::Pending => {
                waiting = Some((header, body, future));
                break;
            }
        }
    }
    assert!(
        !admitted.is_empty(),
        "HWM admitted no packet before refusal"
    );
    let (waiting_header, waiting_body, mut waiting) = waiting.expect("HWM produced no wait token");

    // Transfer completion-queue ownership only after backpressure so an
    // initial writable edge cannot be mistaken for this token's WRITABLE.
    let poller = Poller::new().unwrap();
    poller
        .add_socket(&sender, POLLOUT | POLLCOMPLETION, 41)
        .unwrap();
    let mut events = [PollEvent::default()];
    assert_eq!(poller.wait(&mut events, 0).unwrap(), 0);

    let mut received = Received::empty();
    for (expected_header, expected_body) in &admitted {
        assert!(receiver.recv(&mut received, RecvFlags::NONE).unwrap());
        assert_eq!(received.parts().len(), 2);
        assert_eq!(received.parts()[0].as_bytes(), expected_header);
        assert_eq!(received.parts()[1].as_bytes(), expected_body);
    }
    assert!(!receiver.recv(&mut received, RecvFlags::DONT_WAIT).unwrap());

    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert_eq!(events[0].slot, 41);
    assert_ne!(events[0].revents & POLLOUT, 0);

    // Poller::wait pulled the matching WRITABLE record. The next Future poll
    // retries the retained packet once; successful admission has ID zero and
    // no SEND completion.
    assert_eq!(test_support::poll_once(&mut waiting), Poll::Ready(Ok(())));
    assert!(receiver.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts().len(), 2);
    assert_eq!(received.parts()[0].as_bytes(), waiting_header);
    assert_eq!(received.parts()[1].as_bytes(), waiting_body);
    assert!(!receiver.recv(&mut received, RecvFlags::DONT_WAIT).unwrap());
}

#[test]
fn request_backpressure_retries_after_its_writable_then_receives_reply() {
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
    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    router.bind("inproc://rust-request-writable-hwm").unwrap();
    dealer
        .connect("inproc://rust-request-writable-hwm")
        .unwrap();

    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());

    let mut first = Box::pin(
        dealer
            .request()
            .message(large_filler(b'a'))
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    let mut retry = Box::pin(
        dealer
            .request()
            .message(large_filler(b'b'))
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    assert!(test_support::poll_once(&mut first).is_pending());
    assert!(test_support::poll_once(&mut retry).is_pending());

    let poller = Poller::new().unwrap();
    poller
        .add_socket(&dealer, POLLOUT | POLLCOMPLETION, 51)
        .unwrap();
    let mut events = [PollEvent::default()];

    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes()[0], b'a');
    received
        .reply()
        .message(Message::try_from(b"first-reply").unwrap())
        .submit()
        .unwrap();
    assert!(!router.recv(&mut received, RecvFlags::DONT_WAIT).unwrap());

    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert!(matches!(
        test_support::poll_once(&mut first),
        Poll::Ready(Ok(ref parts)) if parts[0].as_bytes() == b"first-reply"
    ));
    assert!(test_support::poll_once(&mut retry).is_pending());

    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes()[0], b'b');
    received
        .reply()
        .message(Message::try_from(b"retry-reply").unwrap())
        .submit()
        .unwrap();
    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert!(matches!(
        test_support::poll_once(&mut retry),
        Poll::Ready(Ok(ref parts)) if parts[0].as_bytes() == b"retry-reply"
    ));
}

#[test]
fn backpressured_request_resumes_from_runtime_owner_without_repolling() {
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
    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    router
        .bind("inproc://rust-request-writable-runtime-owner")
        .unwrap();
    dealer
        .connect("inproc://rust-request-writable-runtime-owner")
        .unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());

    let mut first = Box::pin(
        dealer
            .request()
            .message(large_filler(b'a'))
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    let mut retry = Box::pin(
        dealer
            .request()
            .message(large_filler(b'b'))
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    assert!(test_support::poll_once(&mut first).is_pending());
    assert!(test_support::poll_once(&mut retry).is_pending());
    let (retry, polls) = counted(retry);
    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(retry)).unwrap();
    });

    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes()[0], b'a');
    received
        .reply()
        .message(Message::try_from(b"first-reply").unwrap())
        .submit()
        .unwrap();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes()[0], b'b');
    received
        .reply()
        .message(Message::try_from(b"runtime-reply").unwrap())
        .submit()
        .unwrap();

    let reply = done_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("runtime owner did not resume the request")
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"runtime-reply");
    waiter.join().unwrap();
    assert!(matches!(
        test_support::poll_once(&mut first),
        Poll::Ready(Ok(ref parts)) if parts[0].as_bytes() == b"first-reply"
    ));
    let polls = polls.load(std::sync::atomic::Ordering::SeqCst);
    assert!(
        polls <= 8,
        "parked REQUEST future was re-polled {polls} times: executor busy loop"
    );
}

#[test]
fn request_connect_before_bind_waits_for_writable_without_sleep() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    dealer.common_options().set_immediate(true).unwrap();
    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    dealer
        .connect("inproc://rust-request-connect-before-bind")
        .unwrap();

    let mut request = Box::pin(
        dealer
            .request()
            .message(Message::try_from(b"before-bind").unwrap())
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    assert!(test_support::poll_once(&mut request).is_pending());

    let poller = Poller::new().unwrap();
    poller
        .add_socket(&dealer, POLLOUT | POLLCOMPLETION, 52)
        .unwrap();
    let mut events = [PollEvent::default()];
    router
        .bind("inproc://rust-request-connect-before-bind")
        .unwrap();
    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert!(test_support::poll_once(&mut request).is_pending());

    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes(), b"before-bind");
    received
        .reply()
        .message(Message::try_from(b"after-bind").unwrap())
        .submit()
        .unwrap();
    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert!(matches!(
        test_support::poll_once(&mut request),
        Poll::Ready(Ok(ref parts)) if parts[0].as_bytes() == b"after-bind"
    ));
}

#[test]
fn closing_a_socket_cleans_up_a_request_wait_token() {
    let ctx = Context::new().unwrap();
    let mut dealer = ctx.dealer_socket().unwrap();
    dealer.common_options().set_immediate(true).unwrap();
    dealer.connect("inproc://rust-request-token-close").unwrap();
    let mut request = Box::pin(
        dealer
            .request()
            .message(Message::try_from(b"close-before-admission").unwrap())
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    assert!(test_support::poll_once(&mut request).is_pending());

    dealer.close().unwrap();
    let error = match test_support::block_on(request) {
        Ok(_) => panic!("closed request must fail"),
        Err(error) => error,
    };
    assert!(matches!(
        error,
        ZlinkError::Submit(submit)
            if submit.code() == SubmitResult::Terminated
                && submit.native_errno() == libc::ESHUTDOWN
    ));
}

#[test]
fn request_and_send_wait_tokens_share_the_completion_lane() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    dealer
        .common_options()
        .set_send_high_water_mark(2 * RECORD_HWM)
        .unwrap();
    router
        .common_options()
        .set_receive_high_water_mark(2 * RECORD_HWM)
        .unwrap();
    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    router.bind("inproc://rust-request-send-token-mix").unwrap();
    dealer
        .connect("inproc://rust-request-send-token-mix")
        .unwrap();

    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());

    for marker in [b'1', b'2'] {
        let result = test_support::block_on(dealer.send().message(large_filler(marker)).submit());
        result.expect("filler admission");
    }
    let mut request = Box::pin(
        dealer
            .request()
            .message(large_filler(b'r'))
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    let mut send = Box::pin(dealer.send().message(large_filler(b's')).submit());
    assert!(test_support::poll_once(&mut request).is_pending());
    assert!(test_support::poll_once(&mut send).is_pending());

    let poller = Poller::new().unwrap();
    poller
        .add_socket(&dealer, POLLOUT | POLLCOMPLETION, 53)
        .unwrap();
    let mut events = [PollEvent::default()];
    for marker in [b'1', b'2'] {
        assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
        assert_eq!(received.parts()[0].as_bytes()[0], marker);
    }
    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert!(test_support::poll_once(&mut request).is_pending());
    assert_eq!(test_support::poll_once(&mut send), Poll::Ready(Ok(())));

    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes()[0], b'r');
    received
        .reply()
        .message(Message::try_from(b"mixed-reply").unwrap())
        .submit()
        .unwrap();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.parts()[0].as_bytes()[0], b's');
    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert!(matches!(
        test_support::poll_once(&mut request),
        Poll::Ready(Ok(ref parts)) if parts[0].as_bytes() == b"mixed-reply"
    ));
}

#[test]
fn dropping_a_pending_send_future_detaches_the_waiter() {
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
    dealer
        .connect("inproc://rust-send-complete-cancel")
        .unwrap();
    thread::sleep(Duration::from_millis(75));

    let pending = saturate(|| Box::pin(dealer.send().message(large_filler(b'd')).submit()));
    assert!(!pending.is_empty(), "test target did not reach HWM");

    let mut cancelled = Box::pin(dealer.send().message(large_filler(b'z')).submit());
    assert_eq!(test_support::poll_once(&mut cancelled), Poll::Pending);
    // Dropping the Future discards the retained payload. Its payload-free sink
    // remains until Core retires the live token or socket cleanup runs.
    drop(cancelled);
    drop(pending);

    let mut received = Received::empty();
    let _ = router.recv(&mut received, RecvFlags::DONT_WAIT);
}

#[test]
fn dropped_send_tokens_do_not_starve_an_existing_request() {
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
    router
        .bind("inproc://rust-send-token-request-progress")
        .unwrap();
    dealer
        .connect("inproc://rust-send-token-request-progress")
        .unwrap();

    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

    let request = dealer
        .request()
        .message(Message::try_from(b"question").unwrap())
        .timeout(Duration::from_secs(3))
        .submit();
    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(request)).unwrap();
    });

    let mut received_request = Received::empty();
    assert!(router.recv(&mut received_request, RecvFlags::NONE).unwrap());
    let pending = saturate(|| Box::pin(dealer.send().message(large_filler(b'q')).submit()));
    assert!(!pending.is_empty(), "test target did not reach HWM");
    drop(pending);

    received_request
        .reply()
        .message(Message::try_from(b"answer").unwrap())
        .submit()
        .unwrap();
    let reply = done_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("detached SEND tokens starved REQUEST progress")
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"answer");
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
    let error = outcome.expect_err("a closed socket cannot retry a retained record");
    // Socket lifecycle cleanup settles the retained SEND locally even when
    // close discards the unread terminal WRITABLE record.
    assert_eq!(error.code(), SubmitResult::Terminated);
    assert_eq!(error.native_errno(), libc::ESHUTDOWN);
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
    // away while target A's binding-owned packet waits for WRITABLE.
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
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

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

#[test]
fn request_sync_return_waits_for_reply() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router.bind("inproc://rust-request-sync-return").unwrap();
    dealer.connect("inproc://rust-request-sync-return").unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

    // The replier hands the ROUTER back so the peer stays open until the
    // reply has been consumed; closing it right after the submit races the
    // inproc delivery of that reply.
    let replier = thread::spawn(move || {
        let mut request = Received::empty();
        assert!(router.recv(&mut request, RecvFlags::NONE).unwrap());
        request
            .reply()
            .message(Message::try_from(b"sync-reply").unwrap())
            .submit()
            .unwrap();
        router
    });
    let reply = dealer
        .request()
        .message(Message::try_from(b"sync-request").unwrap())
        .timeout(Duration::from_secs(2))
        .submit_sync()
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"sync-reply");
    drop(replier.join().unwrap());
}

#[test]
fn dropped_request_future_cleans_up_its_late_completion() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router.bind("inproc://rust-request-late-cleanup").unwrap();
    dealer
        .connect("inproc://rust-request-late-cleanup")
        .unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

    let responder = thread::spawn(move || {
        for payload in [b"late-reply".as_slice(), b"next-reply".as_slice()] {
            let mut request = Received::empty();
            assert!(router.recv(&mut request, RecvFlags::NONE).unwrap());
            request
                .reply()
                .message(Message::try_from(payload).unwrap())
                .submit()
                .unwrap();
        }
        router
    });

    let mut dropped = Box::pin(
        dealer
            .request()
            .message(Message::try_from(b"drop-request").unwrap())
            .timeout(Duration::from_secs(2))
            .submit(),
    );
    assert!(matches!(
        test_support::poll_once(&mut dropped),
        Poll::Pending
    ));
    drop(dropped);
    thread::sleep(Duration::from_millis(50));

    let reply = test_support::block_on(
        dealer
            .request()
            .message(Message::try_from(b"next-request").unwrap())
            .timeout(Duration::from_secs(2))
            .submit(),
    )
    .unwrap();
    assert_eq!(reply[0].as_bytes(), b"next-reply");
    drop(responder.join().unwrap());
}

/// Counts executor polls so a parked SEND/REQUEST future can prove it is
/// resumed by the binding reactor rather than by executor re-polling.
struct PollCounter<F> {
    inner: F,
    polls: std::sync::Arc<std::sync::atomic::AtomicUsize>,
}

impl<F: std::future::Future + Unpin> std::future::Future for PollCounter<F> {
    type Output = F::Output;

    fn poll(
        mut self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> Poll<Self::Output> {
        self.polls.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        std::pin::Pin::new(&mut self.inner).poll(cx)
    }
}

fn counted<F: std::future::Future + Unpin>(
    inner: F,
) -> (
    PollCounter<F>,
    std::sync::Arc<std::sync::atomic::AtomicUsize>,
) {
    let polls = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
    (
        PollCounter {
            inner,
            polls: std::sync::Arc::clone(&polls),
        },
        polls,
    )
}

#[test]
fn backpressured_send_resumes_without_executor_repolls() {
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
    router
        .common_options()
        .set_receive_timeout(Duration::from_secs(3))
        .unwrap();
    router.bind("inproc://rust-send-no-repoll").unwrap();
    dealer.connect("inproc://rust-send-no-repoll").unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());

    let filler = saturate(|| Box::pin(dealer.send().message(large_filler(b'p')).submit()));
    assert!(!filler.is_empty(), "test target did not reach HWM");

    let mut parked = Box::pin(
        dealer
            .send()
            .message(Message::try_from(b"parked-header").unwrap())
            .message(large_filler(b'w'))
            .submit(),
    );
    assert_eq!(test_support::poll_once(&mut parked), Poll::Pending);
    let (parked, polls) = counted(parked);

    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(parked)).unwrap();
    });

    // Drain the receiver until the parked packet arrives; the waiter thread is
    // parked the whole time and must be woken by the reactor, not by polling.
    let mut got_parked = false;
    while !got_parked {
        assert!(
            router.recv(&mut received, RecvFlags::NONE).unwrap(),
            "receiver timed out before the parked packet was delivered"
        );
        got_parked = received.parts()[0].as_bytes() == b"parked-header";
    }
    assert_eq!(received.parts().len(), 2);
    done_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("parked SEND was not resumed by the reactor")
        .unwrap();
    waiter.join().unwrap();
    let polls = polls.load(std::sync::atomic::Ordering::SeqCst);
    assert!(
        polls <= 8,
        "parked SEND future was re-polled {polls} times: executor busy loop"
    );
    drop(filler);
}

#[test]
fn request_alongside_live_send_tokens_is_not_repolled() {
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
    router.bind("inproc://rust-request-no-repoll").unwrap();
    dealer.connect("inproc://rust-request-no-repoll").unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

    let mut request = Box::pin(
        dealer
            .request()
            .message(Message::try_from(b"question").unwrap())
            .timeout(Duration::from_secs(3))
            .submit(),
    );
    assert!(test_support::poll_once(&mut request).is_pending());
    let (request, polls) = counted(request);
    let mut received_request = Received::empty();
    assert!(router.recv(&mut received_request, RecvFlags::NONE).unwrap());

    // Live SEND wait tokens on the same socket must not turn the REQUEST
    // waiter into a polling loop.
    let pending = saturate(|| Box::pin(dealer.send().message(large_filler(b'q')).submit()));
    assert!(!pending.is_empty(), "test target did not reach HWM");

    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(request)).unwrap();
    });
    assert!(done_rx.recv_timeout(Duration::from_millis(100)).is_err());
    received_request
        .reply()
        .message(Message::try_from(b"answer").unwrap())
        .submit()
        .unwrap();
    let reply = done_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("REQUEST was not completed")
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"answer");
    waiter.join().unwrap();
    let polls = polls.load(std::sync::atomic::Ordering::SeqCst);
    assert!(
        polls <= 4,
        "REQUEST future was re-polled {polls} times: executor busy loop"
    );
    drop(pending);
}

#[test]
fn removing_the_target_fails_a_parked_router_send() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let rid = RoutingId::from(b"rust-terminal-target");
    dealer.set_routing_id(&rid).unwrap();
    router
        .common_options()
        .set_send_high_water_mark(RECORD_HWM)
        .unwrap();
    dealer
        .common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    router.bind("inproc://rust-terminal-writable").unwrap();
    dealer.connect("inproc://rust-terminal-writable").unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

    let filler = saturate(|| Box::pin(router.send(&rid).message(large_filler(b't')).submit()));
    assert!(!filler.is_empty(), "test target did not reach HWM");
    let mut parked = Box::pin(
        router
            .send(&rid)
            .message(Message::try_from(b"never-delivered").unwrap())
            .submit(),
    );
    assert_eq!(test_support::poll_once(&mut parked), Poll::Pending);

    let (done_tx, done_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        done_tx.send(test_support::block_on(parked)).unwrap();
    });
    assert!(done_rx.recv_timeout(Duration::from_millis(50)).is_err());

    // Explicit target removal retires the token with a terminal WRITABLE;
    // the waiter must fail instead of waiting forever.
    router.disconnect_rid(&rid).unwrap();
    let outcome = done_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("terminal WRITABLE did not resume the parked send");
    let error = outcome.expect_err("a removed target cannot admit the retained packet");
    assert!(
        error.native_errno() == libc::ENOENT || error.native_errno() == libc::ESHUTDOWN,
        "unexpected terminal errno {}",
        error.native_errno()
    );
    assert!(
        matches!(
            error.code(),
            SubmitResult::NotFound | SubmitResult::Terminated
        ),
        "unexpected terminal code {:?}",
        error.code()
    );
    waiter.join().unwrap();
    drop(filler);
}

#[test]
fn removing_the_target_fails_a_parked_router_request_with_typed_error() {
    let ctx = Context::new().unwrap();
    ctx.options().set_auto_hwm_enabled(false).unwrap();
    let router = ctx.router_socket().unwrap();
    let peer = ctx.router_socket().unwrap();
    let rid = RoutingId::from(b"rust-request-terminal-target");
    peer.set_routing_id(&rid).unwrap();
    router
        .common_options()
        .set_send_high_water_mark(RECORD_HWM)
        .unwrap();
    peer.common_options()
        .set_receive_high_water_mark(RECORD_HWM)
        .unwrap();
    router
        .bind("inproc://rust-request-terminal-writable")
        .unwrap();
    peer.connect("inproc://rust-request-terminal-writable")
        .unwrap();
    router
        .send(&rid)
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(peer.recv(&mut ready, RecvFlags::NONE).unwrap());

    let filler = saturate(|| Box::pin(router.send(&rid).message(large_filler(b't')).submit()));
    assert!(!filler.is_empty(), "test target did not reach HWM");
    let mut request = Box::pin(
        router
            .request(&rid)
            .message(Message::try_from(b"never-admitted").unwrap())
            .timeout(Duration::from_secs(5))
            .submit(),
    );
    assert!(test_support::poll_once(&mut request).is_pending());

    router.disconnect_rid(&rid).unwrap();
    let error = match test_support::block_on(request) {
        Ok(_) => panic!("removed request target must fail"),
        Err(error) => error,
    };
    assert!(matches!(
        error,
        ZlinkError::Submit(submit)
            if matches!(submit.code(), SubmitResult::NotFound | SubmitResult::Terminated)
                && matches!(submit.native_errno(), libc::ENOENT | libc::ESHUTDOWN)
    ));
    drop(filler);
}
