//! Shared multi-socket request/reply benchmark implementation.

use crate::common;
use std::collections::VecDeque;
use std::future::Future;
use std::io::{self, BufRead};
use std::pin::Pin;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};
use zlink::{
    DealerSocket, Message, POLLCOMPLETION, POLLIN, PollEvent, Poller, RecvFlags, RecvResult,
    RouterSocket, RoutingId, SubmitResult, ZlinkError,
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
            pattern: "MULTI_DEALER_ROUTER_REQREP",
            router_clients: false,
            server_has_routing_id: false,
        }
    }

    pub const fn router_router() -> Self {
        Self {
            pattern: "MULTI_ROUTER_ROUTER_REQREP",
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

type RequestTask = Pin<Box<dyn Future<Output = RequestCompletion> + Send>>;

struct LogicalRequest {
    socket_index: usize,
    sequence: u64,
    payload: Vec<u8>,
}

impl LogicalRequest {
    fn new(socket_index: usize, sequence: u64, payload_size: usize, message_size: usize) -> Self {
        let mut payload = vec![0; payload_size];
        common::encode_header(
            &mut payload,
            common::PHASE_ACTIVE,
            message_size as u32,
            sequence,
        );
        Self {
            socket_index,
            sequence,
            payload,
        }
    }
}

struct RequestCompletion {
    request: LogicalRequest,
    outcome: Result<Vec<Message>, ZlinkError>,
}

impl RequestClientSocket {
    fn request_task(&self, request: LogicalRequest, timeout: Duration) -> RequestTask {
        let payload = Message::try_from(request.payload.as_slice()).expect("request payload");

        let operation = match self {
            Self::Dealer(socket) => socket.request(),
            Self::Router { socket, target } => socket.request(target),
        }
        .message(payload);
        let future = if common::measurement_part_count() == 2 {
            operation
                .message(Message::try_from(&[] as &[u8]).expect("empty request tail"))
                .timeout(timeout)
                .submit()
        } else {
            operation.timeout(timeout).submit()
        };

        Box::pin(async move {
            RequestCompletion {
                request,
                outcome: future.await,
            }
        })
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
        .set_send_timeout(Duration::from_millis(settings.send_timeout_ms))
        .expect("send timeout");
    router
        .common_options()
        .set_receive_timeout(Duration::from_millis(settings.receive_timeout_ms))
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
                Ok(true) => reply_request(&received),
                Ok(false) => break,
                Err(error) if error.code() == RecvResult::NoData => break,
                Err(error) => panic!("request receive failed: {error}"),
            }
        }
    }
}

fn reply_request(received: &zlink::Received) {
    if received.routing_id().is_none() || received.request_seq().is_none() {
        panic!("request is missing its reply route");
    }
    let payload = common::message_payload(received.parts());
    if payload.is_empty() {
        panic!("request has an invalid measurement envelope");
    }

    let reply = received
        .reply()
        .message(Message::try_from(payload).expect("reply payload"));
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
    drop(monitors);
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
    let active_deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    let mut sequences = vec![1u64; sockets.len()];
    let mut retry_requests: Vec<VecDeque<LogicalRequest>> =
        (0..sockets.len()).map(|_| VecDeque::new()).collect();
    let mut requests = common::ConcurrentTasks::<RequestTask>::new(0);
    let mut latency = common::LatencyStats::new();

    // There is deliberately no per-socket or global application window. Each
    // turn adds a request per socket, and Core admission plus the request
    // Futures themselves determine how many remain in flight.
    while Instant::now() < active_deadline {
        let mut new_tasks = Vec::with_capacity(sockets.len());
        for (socket_index, socket) in sockets.iter().enumerate() {
            if Instant::now() >= active_deadline {
                break;
            }
            let request = retry_requests[socket_index].pop_front().unwrap_or_else(|| {
                let sequence = sequences[socket_index];
                sequences[socket_index] = sequence.wrapping_add(1);
                LogicalRequest::new(socket_index, sequence, payload_size, args.msg_size)
            });
            let task_slot = requests.push(socket.request_task(request, request_timeout));
            new_tasks.push(task_slot);
        }

        let ready = requests.poll_ready();
        let mut progressed = !ready.is_empty();
        for (_, completion) in ready {
            if let Some(retry) =
                process_completion(completion, args.msg_size, active_deadline, &mut latency)
            {
                enqueue_retry(&mut retry_requests, retry);
            }
        }
        progressed |= new_tasks
            .iter()
            .any(|task_slot| requests.is_pending(*task_slot));
        if Instant::now() < active_deadline {
            // Core queues request callbacks on the socket completion lane.
            // Even after executor progress, a zero-time wait must drain that
            // lane so the callback can wake its Future. Only a genuinely idle
            // turn blocks, and then only until the active deadline.
            let wait_ms = if progressed {
                0
            } else {
                common::poll_timeout_until(active_deadline)
            };
            completion_poller
                .wait(&mut completion_events, wait_ms)
                .expect("request completion wait");
        }
    }

    let drain_deadline =
        Instant::now() + common::resolve_multi_reqrep_drain_timeout(request_timeout);
    while requests.any_pending() && Instant::now() < drain_deadline {
        let ready = requests.poll_ready();
        let progressed = !ready.is_empty();
        for (_, completion) in ready {
            let _ = process_completion(completion, args.msg_size, active_deadline, &mut latency);
        }
        if requests.any_pending() {
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
    for (_, completion) in requests.poll_ready() {
        let _ = process_completion(completion, args.msg_size, active_deadline, &mut latency);
    }
    assert!(
        !requests.any_pending(),
        "request completion drain timed out"
    );

    common::print_result(
        config.pattern,
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &latency.finish(),
    );
}

fn configure_client_options<S>(socket: &S, settings: &common::MultiSettings)
where
    S: ClientCommonOptions,
{
    socket
        .set_send_timeout(Duration::from_millis(settings.send_timeout_ms))
        .expect("send timeout");
    socket
        .set_receive_timeout(Duration::from_millis(settings.receive_timeout_ms))
        .expect("recv timeout");
}

trait ClientCommonOptions {
    fn set_send_timeout(&self, timeout: Duration) -> Result<(), ZlinkError>;
    fn set_receive_timeout(&self, timeout: Duration) -> Result<(), ZlinkError>;
}

macro_rules! impl_client_common_options {
    ($($socket:ty),+ $(,)?) => {
        $(
            impl ClientCommonOptions for $socket {
                fn set_send_timeout(&self, timeout: Duration) -> Result<(), ZlinkError> {
                    Ok(self.common_options().set_send_timeout(timeout)?)
                }

                fn set_receive_timeout(&self, timeout: Duration) -> Result<(), ZlinkError> {
                    Ok(self.common_options().set_receive_timeout(timeout)?)
                }
            }
        )+
    };
}

impl_client_common_options!(DealerSocket, RouterSocket);

fn process_completion(
    completion: RequestCompletion,
    message_size: usize,
    active_deadline: Instant,
    latency: &mut common::LatencyStats,
) -> Option<LogicalRequest> {
    let RequestCompletion { request, outcome } = completion;
    match outcome {
        Ok(parts) => {
            if Instant::now() < active_deadline {
                let payload = common::message_payload(&parts);
                common::record_active_rtt_latency(payload, message_size, latency);
            }
            None
        }
        Err(error) if retryable_pre_admission(&error) => {
            (Instant::now() < active_deadline).then_some(request)
        }
        // C's request callback treats request-domain completion failures as
        // completed operations and continues driving the active window.
        Err(ZlinkError::Request(_)) => None,
        Err(error) => panic!("request failed: {error}"),
    }
}

fn retryable_pre_admission(error: &ZlinkError) -> bool {
    matches!(
        error,
        ZlinkError::Submit(submit)
            if matches!(
                submit.code(),
                SubmitResult::Backpressured | SubmitResult::NotAdmitted
            )
    )
}

fn enqueue_retry(retry_requests: &mut [VecDeque<LogicalRequest>], retry: LogicalRequest) {
    let socket_index = retry.socket_index;
    retry_requests[socket_index].push_back(retry);
}

#[cfg(test)]
mod tests {
    use super::*;
    use zlink::SubmitError;

    #[test]
    fn official_configs_keep_distinct_request_roles() {
        let dealer_router = ReqRepConfig::dealer_router();
        assert_eq!(dealer_router.pattern, "MULTI_DEALER_ROUTER_REQREP");
        assert!(!dealer_router.router_clients);
        assert!(!dealer_router.server_has_routing_id);

        let router_router = ReqRepConfig::router_router();
        assert_eq!(router_router.pattern, "MULTI_ROUTER_ROUTER_REQREP");
        assert!(router_router.router_clients);
        assert!(router_router.server_has_routing_id);
    }

    #[test]
    fn only_pre_admission_pressure_is_resubmitted() {
        assert!(retryable_pre_admission(&ZlinkError::Submit(
            SubmitError::new(SubmitResult::Backpressured, libc::EAGAIN)
        )));
        assert!(retryable_pre_admission(&ZlinkError::Submit(
            SubmitError::new(SubmitResult::NotAdmitted, libc::EAGAIN)
        )));
        assert!(!retryable_pre_admission(&ZlinkError::Submit(
            SubmitError::new(SubmitResult::NotConnected, libc::ENOTCONN)
        )));
    }

    #[test]
    fn pre_admission_retry_preserves_the_logical_payload() {
        let request = LogicalRequest::new(3, 17, common::HEADER_SIZE, common::HEADER_SIZE);
        let payload = request.payload.clone();
        let completion = RequestCompletion {
            request,
            outcome: Err(SubmitError::new(SubmitResult::Backpressured, libc::EAGAIN).into()),
        };
        let mut latency = common::LatencyStats::new();
        let retry = process_completion(
            completion,
            common::HEADER_SIZE,
            Instant::now() + Duration::from_secs(1),
            &mut latency,
        )
        .expect("retryable pre-admission failure");

        assert_eq!(retry.socket_index, 3);
        assert_eq!(retry.sequence, 17);
        assert_eq!(retry.payload, payload);
    }

    #[test]
    fn multiple_pre_admission_retries_for_one_socket_are_preserved_in_order() {
        let mut retry_requests: Vec<VecDeque<LogicalRequest>> =
            (0..4).map(|_| VecDeque::new()).collect();
        let mut expected_payloads = Vec::new();
        let mut latency = common::LatencyStats::new();

        for sequence in [17, 18] {
            let request =
                LogicalRequest::new(3, sequence, common::HEADER_SIZE, common::HEADER_SIZE);
            expected_payloads.push(request.payload.clone());
            let retry = process_completion(
                RequestCompletion {
                    request,
                    outcome: Err(SubmitError::new(SubmitResult::NotAdmitted, libc::EAGAIN).into()),
                },
                common::HEADER_SIZE,
                Instant::now() + Duration::from_secs(1),
                &mut latency,
            )
            .expect("retryable pre-admission failure");
            enqueue_retry(&mut retry_requests, retry);
        }

        assert_eq!(retry_requests[3].len(), 2);
        for (sequence, expected_payload) in [17, 18].into_iter().zip(expected_payloads) {
            let retry = retry_requests[3].pop_front().expect("queued retry");
            assert_eq!(retry.sequence, sequence);
            assert_eq!(retry.payload, expected_payload);
        }
        assert!(retry_requests[3].is_empty());
    }
}
