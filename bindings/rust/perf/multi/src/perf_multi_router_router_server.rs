#[path = "perf_common.rs"]
mod common;

use std::future::Future;
use std::io::{self, BufRead};
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::time::{Duration, Instant};
use zlink::{
    Message, POLLCOMPLETION, POLLIN, PollEvent, Poller, RecvFlags, RecvResult, RoutingId,
    SubmitError, SubmitResult,
};

fn finish_ready_replies<F>(tasks: &mut common::ConcurrentTasks<F>)
where
    F: Future<Output = Result<(), SubmitError>>,
{
    for (_, result) in tasks.poll_ready() {
        match result {
            Ok(()) => {}
            Err(err)
                if matches!(
                    err.code(),
                    SubmitResult::NotConnected | SubmitResult::NotFound
                ) => {}
            Err(err) => panic!("routed reply failed: {err}"),
        }
    }
}

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();
    let ctx = common::perf_server_context();
    let router = ctx.router_socket().expect("router");
    let rid = RoutingId::from(b"perf-rr-server");
    router.set_routing_id(&rid).expect("set rid");
    // C parity: numeric HWM remains behind the manual-override gate.
    common::apply_multi_hwm(&router, &settings);
    router
        .common_options()
        .set_send_timeout(Duration::from_millis(settings.send_timeout_ms))
        .expect("send timeout");
    router
        .common_options()
        .set_receive_timeout(Duration::from_millis(1))
        .expect("recv timeout");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&router, &tls).expect("server tls");
    }
    let Some(bind_endpoint) =
        common::resolve_server_bind_endpoint("MULTI_ROUTER_ROUTER_SENDSEND", &args.transport)
    else {
        return;
    };
    if let Err(err) = router.bind(&bind_endpoint) {
        if common::handle_transport_setup_error(
            "MULTI_ROUTER_ROUTER_SENDSEND",
            &args.transport,
            "bind",
            err,
        ) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = router.last_endpoint().expect("endpoint");
    common::print_ready(&endpoint);
    let stop = Arc::new(AtomicBool::new(false));
    let stop_reader = stop.clone();
    std::thread::spawn(move || {
        let stdin = io::stdin();
        for line in stdin.lock().lines() {
            let line = line.unwrap_or_default();
            if matches!(line.trim(), "STOP" | "QUIT") {
                stop_reader.store(true, Ordering::Release);
                break;
            }
        }
    });
    // Keep routed send Futures alive across receive bursts. POLLCOMPLETION
    // dispatches their Core completions on the same signal-driven wait that
    // drains POLLIN, so a backpressured reply never stops receive progress.
    let poller = Poller::new().expect("poller");
    poller
        .add_socket(&router, POLLIN | POLLCOMPLETION, 0)
        .expect("poller add");
    let mut events = vec![PollEvent::default(); 1];
    let mut received = zlink::Received::empty();
    let mut replies = common::ConcurrentTasks::new(0);
    while !stop.load(Ordering::Acquire) {
        // Bound the idle wait so the control thread's STOP/QUIT request can
        // terminate the server even when no request is queued.
        let event_count = match poller.wait(&mut events, 100) {
            Ok(event_count) => event_count,
            Err(err) => panic!("poller wait failed: {err}"),
        };
        if stop.load(Ordering::Acquire) {
            break;
        }

        for event in &events[..event_count] {
            if event.slot != 0 {
                continue;
            }

            if event.revents & POLLIN != 0 {
                loop {
                    match router.recv(&mut received, RecvFlags::DONT_WAIT) {
                        Ok(true) => {
                            let Some(rid) = received.routing_id().cloned() else {
                                continue;
                            };
                            let reply_bytes = common::message_payload(received.parts()).to_vec();
                            let router_ref = &router;
                            replies.push(async move {
                                let msg = Message::try_from(reply_bytes.as_slice()).expect("reply");
                                perf_submit_measurement_async!(router_ref.send(&rid), msg).await
                            });
                            // Enter Core before draining the next request. A
                            // continuously readable socket must not leave the
                            // queued Rust Futures inert until recv reaches
                            // NoData.
                            finish_ready_replies(&mut replies);
                        }
                        Ok(false) => break,
                        Err(err) if err.code() == RecvResult::NoData => break,
                        Err(err) => panic!("router recv failed: {err}"),
                    }
                }
            }
        }
        finish_ready_replies(&mut replies);
    }

    // STOP is runner teardown, not part of the measured data path. Let already
    // submitted replies finish, but never let a dead route hold teardown past
    // the configured send timeout; dropping a remaining Future cancels it.
    let drain_deadline = Instant::now() + Duration::from_millis(settings.send_timeout_ms.max(1));
    while replies.any_pending() && Instant::now() < drain_deadline {
        finish_ready_replies(&mut replies);
        if !replies.any_pending() {
            break;
        }
        let wait_ms = drain_deadline
            .saturating_duration_since(Instant::now())
            .as_millis()
            .clamp(1, i64::MAX as u128) as i64;
        match poller.wait(&mut events, wait_ms) {
            Ok(_) => {}
            Err(err) => panic!("reply drain poll failed: {err}"),
        }
    }
    finish_ready_replies(&mut replies);
}
