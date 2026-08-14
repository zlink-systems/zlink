#[path = "perf_common.rs"]
mod common;

use std::collections::VecDeque;
use std::io::{self, BufRead};
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::time::Duration;
use zlink::{
    Message, POLLIN, POLLOUT, PollEvent, Poller, RecvFlags, RecvResult, RoutingId, SendFlags,
    SubmitResult,
};

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();
    let ctx = common::perf_server_context();
    let router = ctx.router_socket().expect("router");
    // C parity: numeric HWM remains behind the manual-override gate.
    common::apply_multi_hwm(&router, &settings);
    router
        .common_options()
        .set_receive_timeout(Duration::from_millis(settings.receive_timeout_ms))
        .expect("recv timeout");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&router, &tls).expect("server tls");
    }
    let Some(bind_endpoint) =
        common::resolve_server_bind_endpoint("MULTI_DEALER_ROUTER", &args.transport)
    else {
        return;
    };
    if let Err(err) = router.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("MULTI_DEALER_ROUTER", &args.transport, "bind", err)
        {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = router.last_endpoint().expect("endpoint");
    common::print_ready(&endpoint);
    let mut pending = VecDeque::<(RoutingId, Vec<u8>)>::new();
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
    // C perf_multi_relay_server.hpp run_server_loop(): poller POLLIN (+ POLLOUT
    // when there is pending backlog), perf_socket_poll(...,-1) signal-driven.
    // No DONT_WAIT busy-spin with sleep(1ms). Real send errors propagate (we
    // panic instead of silently swallowing the data-path send).
    let poller = Poller::new().expect("poller");
    poller.add_socket(&router, POLLIN, 0).expect("poller add");
    let mut events = vec![PollEvent::default(); 1];
    let mut want_pollout = false;
    let mut received = zlink::Received::empty();
    while !stop.load(Ordering::Acquire) {
        let desired = if pending.is_empty() {
            POLLIN
        } else {
            POLLIN | POLLOUT
        };
        if desired
            != (if want_pollout {
                POLLIN | POLLOUT
            } else {
                POLLIN
            })
        {
            poller
                .modify_socket(&router, desired)
                .expect("poller modify");
            want_pollout = !pending.is_empty();
        }
        // Bound the idle wait so the control thread's STOP/QUIT request can
        // terminate the server even when no request is queued.
        match poller.wait(&mut events, 100) {
            Ok(_) => {}
            Err(err) => panic!("poller wait failed: {err}"),
        }
        if stop.load(Ordering::Acquire) {
            break;
        }
        // Flush pending backlog first (POLLOUT side).
        while let Some((rid, reply_bytes)) = pending.pop_front() {
            match common::block_on(
                router
                    .send(&rid)
                    .message(Message::try_from(&reply_bytes).expect("pending reply"))
                    .submit(),
            ) {
                Ok(()) => {}
                Err(err) if err.code() == SubmitResult::Backpressured => {
                    pending.push_front((rid, reply_bytes));
                    break;
                }
                Err(err) => panic!("pending reply failed: {err}"),
            }
        }
        // Drain readable requests and relay (POLLIN side).
        loop {
            match router.recv(&mut received, RecvFlags::DONT_WAIT) {
                Ok(true) => {
                    let Some(rid) = received.routing_id().cloned() else {
                        continue;
                    };
                    let reply_bytes = common::message_payload(received.parts()).to_vec();
                    if pending.is_empty() {
                        match received
                            .send()
                            .message(Message::try_from(&reply_bytes).expect("reply"))
                            .flags(SendFlags::DONT_WAIT)
                            .submit()
                        {
                            Ok(true) => continue,
                            Ok(false) => {}
                            Err(err) => panic!("received send failed: {err}"),
                        }
                    }
                    pending.push_back((rid, reply_bytes));
                }
                Ok(false) => break,
                Err(err) if err.code() == RecvResult::NoData => break,
                Err(err) => panic!("router recv failed: {err}"),
            }
        }
    }
}
