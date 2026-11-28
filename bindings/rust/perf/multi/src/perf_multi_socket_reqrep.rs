//! Shared multi-socket request/reply benchmark implementation.

use crate::common;
use std::future::Future;
use std::io::{self, BufRead, Write};
use std::pin::Pin;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};
use zlink::{
    DealerSocket, Message, POLLCOMPLETION, POLLIN, PollEvent, Poller, RecvFlags, RecvResult,
    RequestSubmission, RouterSocket, RoutingId, SubmitError, SubmitResult, ZlinkError,
};

const SERVER_ROUTING_ID: &[u8] = b"SERVER";

#[derive(Clone, Copy)]
pub struct ReqRepConfig {
    pattern: &'static str,
    router_clients: bool,
    server_has_routing_id: bool,
}

impl ReqRepConfig {
    pub const fn dealer_router() -> Self {
        Self {
            pattern: "DEALER_ROUTER_REQREP",
            router_clients: false,
            server_has_routing_id: false,
        }
    }

    pub const fn router_router() -> Self {
        Self {
            pattern: "ROUTER_ROUTER_REQREP",
            router_clients: true,
            server_has_routing_id: true,
        }
    }
}

enum RequestClientSocket {
    Dealer(DealerSocket),
    Router {
        socket: RouterSocket,
        target: RoutingId,
    },
}

type RequestTask = Pin<Box<dyn Future<Output = (usize, Result<Vec<Message>, ZlinkError>)> + Send>>;
type AdmissionTask = Pin<Box<dyn Future<Output = Result<(), SubmitError>> + Send>>;

impl RequestClientSocket {
    fn submit_request(
        &self,
        payload: Message,
        timeout: Duration,
    ) -> Result<RequestSubmission, ZlinkError> {
        let operation = match self {
            Self::Dealer(socket) => socket.request(),
            Self::Router { socket, target } => socket.request(target),
        }
        .message(payload);
        if common::measurement_part_count() == 2 {
            operation
                .message(Message::try_from(&[] as &[u8]).expect("empty request tail"))
                .timeout(timeout)
                .submit()
        } else {
            operation.timeout(timeout).submit()
        }
    }
}

pub fn run_server(config: ReqRepConfig) {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();
    let ctx = common::perf_server_context();
    let router = ctx.router_socket().expect("router");
    if config.server_has_routing_id {
        router
            .set_routing_id(&RoutingId::from(SERVER_ROUTING_ID))
            .expect("set server routing id");
    }
    common::apply_multi_hwm(&router, &settings);
    router
        .common_options()
        .set_send_timeout(settings.send_timeout_ms)
        .expect("send timeout");
    router
        .common_options()
        .set_receive_timeout(settings.receive_timeout_ms)
        .expect("recv timeout");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&router, &tls).expect("server tls");
    }

    let Some(bind_endpoint) = common::resolve_server_bind_endpoint(config.pattern, &args.transport)
    else {
        return;
    };
    if let Err(error) = router.bind(&bind_endpoint) {
        if common::handle_transport_setup_error(config.pattern, &args.transport, "bind", error) {
            return;
        }
        panic!("bind: {error}");
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");
    common::print_ready(&router.last_endpoint().expect("endpoint"));

    let stop = Arc::new(AtomicBool::new(false));
    let stop_reader = Arc::clone(&stop);
    thread::spawn(move || {
        let stdin = io::stdin();
        for line in stdin.lock().lines() {
            match line {
                Ok(line) if matches!(line.trim(), "STOP" | "QUIT") => break,
                Ok(_) => {}
                Err(_) => break,
            }
        }
        stop_reader.store(true, Ordering::Release);
    });

    let poller = Poller::new().expect("poller");
    poller.add_socket(&router, POLLIN, 0).expect("poller add");
    let mut events = vec![PollEvent::default(); 1];
    let mut received = zlink::Received::empty();
    while !stop.load(Ordering::Acquire) {
        let event_count = poller.wait(&mut events, 100).expect("poller wait");
        if stop.load(Ordering::Acquire) {
            break;
        }
        if events[..event_count]
            .iter()
            .all(|event| event.slot != 0 || event.revents & POLLIN == 0)
        {
            continue;
        }

        while !stop.load(Ordering::Acquire) {
            match router.recv(&mut received, RecvFlags::DONT_WAIT) {
                Ok(true) => reply_request(std::mem::take(&mut received)),
                Ok(false) => break,
                Err(error) if error.code() == RecvResult::NoData => break,
                Err(error) => panic!("request receive failed: {error}"),
            }
        }
    }
}

fn reply_request(received: zlink::Received) {
    if received.routing_id().is_none() || received.reply_token().is_none() {
        panic!("request is missing its reply route");
    }
    let payload = common::message_payload(received.parts());
    if payload.is_empty() {
        panic!("request has an invalid measurement envelope");
    }

    let reply = received.reply();
    let reply_payload = received
        .into_parts()
        .into_iter()
        .next()
        .expect("received reply payload");
    let reply = reply.message(reply_payload);
    let result = if common::measurement_part_count() == 2 {
        reply
            .message(Message::try_from(&[] as &[u8]).expect("empty reply tail"))
            .submit()
    } else {
        reply.submit()
    };
    match result {
        Ok(()) => {}
        Err(error)
            if matches!(
                error.code(),
                SubmitResult::NotConnected | SubmitResult::NotFound
            ) => {}
        Err(error) => panic!("request reply failed: {error}"),
    }
}

pub fn run_client(config: ReqRepConfig) {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();
    let ctx = common::perf_client_context();
    let server_routing_id = RoutingId::from(SERVER_ROUTING_ID);
    let mut sockets = Vec::with_capacity(settings.clients);
    let mut monitors = Vec::with_capacity(settings.clients);

    for index in 0..settings.clients {
        let routing_id = RoutingId::from(format!("CLIENT-{index}").as_bytes());
        if config.router_clients {
            let socket = ctx.router_socket().expect("router client");
            common::apply_multi_hwm(&socket, &settings);
            configure_client_options(&socket, &settings);
            socket
                .set_routing_id(&routing_id)
                .expect("set client routing id");
            socket
                .router_options()
                .set_connect_routing_id(&server_routing_id)
                .expect("set connect routing id");
            if matches!(args.transport.as_str(), "tls" | "wss") {
                let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
                common::setup_raw_tls_client(&socket, &tls).expect("client tls");
            }
            let monitor = common::open_connection_ready_monitor(&socket);
            socket.connect(&args.endpoint).expect("connect");
            sockets.push(RequestClientSocket::Router {
                socket,
                target: server_routing_id,
            });
            monitors.push(monitor);
        } else {
            let socket = ctx.dealer_socket().expect("dealer client");
            common::apply_multi_hwm(&socket, &settings);
            configure_client_options(&socket, &settings);
            socket
                .set_routing_id(&routing_id)
                .expect("set client routing id");
            if matches!(args.transport.as_str(), "tls" | "wss") {
                let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
                common::setup_raw_tls_client(&socket, &tls).expect("client tls");
            }
            let monitor = common::open_connection_ready_monitor(&socket);
            socket.connect(&args.endpoint).expect("connect");
            sockets.push(RequestClientSocket::Dealer(socket));
            monitors.push(monitor);
        }
    }

    let ready_timeout = common::resolve_multi_connect_ready_timeout();
    for monitor in &mut monitors {
        common::wait_monitor_ready(monitor, ready_timeout, "multi request client");
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");

    let completion_poller = Poller::new().expect("request completion poller");
    for (slot, socket) in sockets.iter().enumerate() {
        match socket {
            RequestClientSocket::Dealer(socket) => completion_poller
                .add_socket(socket, POLLCOMPLETION, slot)
                .expect("request completion poller add"),
            RequestClientSocket::Router { socket, .. } => completion_poller
                .add_socket(socket, POLLCOMPLETION, slot)
                .expect("request completion poller add"),
        }
    }
    let mut completion_events = vec![PollEvent::default(); sockets.len().max(1)];

    let request_timeout = common::resolve_multi_reqrep_timeout();
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
    let active_seconds = settings.duration_seconds;
    let latency_seconds = common::latency_seconds(active_seconds);
    let mut active_received = 0;
    let mut active_sends = 0;
    let mut schedule = None;
    let mut pending_due = vec![0; sockets.len()];
    let result = loop {
        let paced = active_received > 0;
        let end = common::now_ns()
            + if paced {
                common::SETTLE_NS + latency_seconds * common::SECOND_NS
            } else {
                active_seconds * common::SECOND_NS
            };
        if paced {
            schedule = Some(common::Schedule::new(
                end - common::SETTLE_NS - latency_seconds * common::SECOND_NS,
                active_received,
                active_seconds,
            ));
        }
        let mut sender = 0;
        let mut sequences = vec![1u64; sockets.len()];
        let mut requests = common::ConcurrentTasks::<RequestTask>::new(0);
        let mut admissions = common::ConcurrentTasks::<AdmissionTask>::new(sockets.len());
        let mut latency = common::LatencyStats::new();
        let mut paced_received = 0;
        let sample_start = schedule.as_ref().map(|s| s.epoch + common::SETTLE_NS);

        // Submit once per socket per turn, then progress both admission and reply
        // completions. This preserves continuous load without starving the poller.
        while common::now_ns() < end {
            let mut progressed = false;
            for _ in 0..sockets.len() {
                let now = common::now_ns();
                if now >= end || schedule.as_ref().is_some_and(|schedule| !schedule.due(now)) {
                    break;
                }
                let socket_index = sender;
                sender = (sender + 1) % sockets.len();
                let socket = &sockets[socket_index];
                if admissions.is_pending(socket_index) {
                    continue;
                }
                let sequence = sequences[socket_index];
                sequences[socket_index] = sequence.wrapping_add(1);
                let mut payload = Message::with_size(payload_size).expect("request payload");
                common::encode_header(
                    payload.data_mut(),
                    match &schedule {
                        Some(schedule) if now < schedule.epoch + common::SETTLE_NS => {
                            common::PHASE_WARMUP
                        }
                        Some(_) => common::PHASE_LATENCY,
                        None => common::PHASE_ACTIVE,
                    },
                    args.msg_size as u32,
                    sequence,
                );
                let due = schedule.as_mut().map_or(0, |schedule| schedule.reserve());
                let submission = socket
                    .submit_request(payload, request_timeout)
                    .unwrap_or_else(|error| panic!("request submit failed: {error}"));
                let result = submission.result;
                requests.push(Box::pin(
                    async move { (socket_index, submission.reply.await) },
                ));
                progressed = true;
                if result == SubmitResult::Backpressured {
                    pending_due[socket_index] = due;
                    admissions.insert(socket_index, submission.admitted);
                } else if let Some(schedule) = &mut schedule {
                    schedule.admit(due, common::now_ns());
                } else if common::now_ns() < end {
                    active_sends += 1;
                }
            }

            for (index, result) in admissions.poll_ready() {
                result.unwrap_or_else(|error| panic!("request admission failed: {error}"));
                let now = common::now_ns();
                if let Some(schedule) = &mut schedule {
                    schedule.admit(pending_due[index], now);
                } else if now < end {
                    active_sends += 1;
                }
                progressed = true;
            }
            let ready = requests.poll_ready();
            progressed |= !ready.is_empty();
            for (_, (_, completion)) in ready {
                paced_received +=
                    process_completion(completion, args.msg_size, end, sample_start, &mut latency);
            }
            if common::now_ns() < end {
                let wait_ms = if progressed {
                    0
                } else {
                    let now = common::now_ns();
                    let until = schedule
                        .as_ref()
                        .filter(|schedule| !schedule.due(now))
                        .map_or(end, |schedule| end.min(schedule.next()));
                    common::wait_ms(now, until)
                };
                completion_poller
                    .wait(&mut completion_events, wait_ms)
                    .expect("request completion wait");
            }
        }

        let drain_deadline =
            Instant::now() + common::resolve_multi_reqrep_drain_timeout(request_timeout);
        while (requests.any_pending() || admissions.any_pending())
            && Instant::now() < drain_deadline
        {
            let admission_ready = admissions.poll_ready();
            for (index, result) in &admission_ready {
                result
                    .as_ref()
                    .unwrap_or_else(|error| panic!("request admission failed: {error}"));
                if let Some(schedule) = &mut schedule {
                    schedule.admit(pending_due[*index], common::now_ns());
                }
            }
            let ready = requests.poll_ready();
            let progressed = !ready.is_empty() || !admission_ready.is_empty();
            for (_, (_, completion)) in ready {
                paced_received +=
                    process_completion(completion, args.msg_size, end, sample_start, &mut latency);
            }
            if requests.any_pending() || admissions.any_pending() {
                let wait_ms = if progressed {
                    0
                } else {
                    common::poll_timeout_until(drain_deadline)
                };
                completion_poller
                    .wait(&mut completion_events, wait_ms)
                    .expect("request completion drain wait");
            }
        }
        for (_, (_, completion)) in requests.poll_ready() {
            paced_received +=
                process_completion(completion, args.msg_size, end, sample_start, &mut latency);
        }
        assert!(
            !requests.any_pending() && !admissions.any_pending(),
            "request completion drain timed out"
        );

        let mut result = latency.finish();
        if let Some(schedule) = &schedule {
            schedule.print(
                config.pattern,
                args.msg_size,
                (active_sends, active_received, active_seconds),
                paced_received,
                latency_seconds,
                None,
            );
            result.count = active_received;
            break result;
        }
        active_received = result.count;
        assert!(active_received > 0, "no active replies");
    };
    if let Some(socket) = sockets.first() {
        match socket {
            RequestClientSocket::Dealer(socket) => common::print_multi_auto_hwm_detail(
                socket,
                "endpoint",
                &args.transport,
                args.msg_size,
                "dealer",
            ),
            RequestClientSocket::Router { socket, .. } => common::print_multi_auto_hwm_detail(
                socket,
                "endpoint",
                &args.transport,
                args.msg_size,
                "router",
            ),
        }
    }

    common::print_result(
        config.pattern,
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &result,
    );
    // PERF_MULTI_TEST_POLICY.md:379,386-388 / PERF_POLICY.md:483-486 - emit
    // CLIENT_DONE, keep the request completion target sockets open, and close
    // them only after the runner has stopped the server and sent STOP.
    // C reference: bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:792,:722-731.
    println!("CLIENT_DONE,{}", args.msg_size);
    io::stdout().flush().ok();
    if !wait_for_runner_stop_after_done() {
        panic!("runner closed stdin before STOP after CLIENT_DONE");
    }
}

/// Blocks until the runner sends STOP/QUIT after CLIENT_DONE.
/// Mirrors bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:722-731.
fn wait_for_runner_stop_after_done() -> bool {
    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        match line {
            Ok(line) if matches!(line.trim(), "STOP" | "QUIT") => return true,
            Ok(_) => {}
            Err(_) => return false,
        }
    }
    false
}

fn configure_client_options<S>(socket: &S, settings: &common::MultiSettings)
where
    S: ClientCommonOptions,
{
    socket
        .set_send_timeout(settings.send_timeout_ms)
        .expect("send timeout");
    socket
        .set_receive_timeout(settings.receive_timeout_ms)
        .expect("recv timeout");
}

trait ClientCommonOptions {
    fn set_send_timeout(&self, timeout: i32) -> Result<(), ZlinkError>;
    fn set_receive_timeout(&self, timeout: i32) -> Result<(), ZlinkError>;
}

macro_rules! impl_client_common_options {
    ($($socket:ty),+ $(,)?) => {
        $(
            impl ClientCommonOptions for $socket {
                fn set_send_timeout(&self, timeout: i32) -> Result<(), ZlinkError> {
                    Ok(self.common_options().set_send_timeout(timeout)?)
                }

                fn set_receive_timeout(&self, timeout: i32) -> Result<(), ZlinkError> {
                    Ok(self.common_options().set_receive_timeout(timeout)?)
                }
            }
        )+
    };
}

impl_client_common_options!(DealerSocket, RouterSocket);

fn process_completion(
    outcome: Result<Vec<Message>, ZlinkError>,
    message_size: usize,
    end: u64,
    sample_start: Option<u64>,
    latency: &mut common::LatencyStats,
) -> u64 {
    match outcome {
        Ok(parts) => {
            let observed = common::now_ns();
            let payload = common::message_payload(&parts);
            u64::from(common::record_measurement_receive(
                payload,
                message_size,
                observed,
                end,
                sample_start,
                2.0,
                latency,
            ))
        }
        // Request-domain failures are terminal outcomes.
        Err(ZlinkError::Request(_)) => 0,
        Err(error) => panic!("request failed: {error}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn official_configs_keep_distinct_request_roles() {
        let dealer_router = ReqRepConfig::dealer_router();
        assert_eq!(dealer_router.pattern, "DEALER_ROUTER_REQREP");
        assert!(!dealer_router.router_clients);
        assert!(!dealer_router.server_has_routing_id);

        let router_router = ReqRepConfig::router_router();
        assert_eq!(router_router.pattern, "ROUTER_ROUTER_REQREP");
        assert!(router_router.router_clients);
        assert!(router_router.server_has_routing_id);
    }
}
