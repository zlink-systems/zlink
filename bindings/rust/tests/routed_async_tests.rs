//! WRITABLE retry contracts for asynchronous SEND and Core completion
//! contracts for REQUEST.
//!
//! `submit()` retains the logical SEND packet across DONTWAIT backpressure and
//! retries it only after the matching WRITABLE token is pulled. REQUEST reply
//! and deadline completion remain Core-owned.

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

    let replier = thread::spawn(move || {
        let mut request = Received::empty();
        assert!(router.recv(&mut request, RecvFlags::NONE).unwrap());
        request
            .reply()
            .message(Message::try_from(b"sync-reply").unwrap())
            .submit()
            .unwrap();
    });
    let reply = dealer
        .request()
        .message(Message::try_from(b"sync-request").unwrap())
        .timeout(Duration::from_secs(2))
        .submit_sync()
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"sync-reply");
    replier.join().unwrap();
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
    responder.join().unwrap();
}
