//! DEALER-DEALER multi client: one-way active sender with N dealer sockets.

#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead, Write};
use zlink::{DealerSocket, Message, POLLCOMPLETION, PollEvent, Poller, RoutingId, SocketMonitor};

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    let mut sockets: Vec<DealerSocket> = Vec::with_capacity(settings.clients);
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for index in 0..settings.clients {
        let sock = ctx.dealer_socket().expect("dealer");
        // C parity: numeric HWM remains behind the manual-override gate.
        common::apply_multi_hwm(&sock, &settings);
        let routing_id = RoutingId::from(format!("client_{index}").as_bytes());
        sock.set_routing_id(&routing_id).expect("routing id");
        sock.common_options()
            .set_receive_timeout(settings.receive_timeout_ms)
            .expect("rcvtimeo");
        if matches!(args.transport.as_str(), "tls" | "wss") {
            let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
            common::setup_raw_tls_client(&sock, &tls).expect("client tls");
        }
        let mon = common::open_connection_ready_monitor(&sock);
        sock.connect(&args.endpoint).expect("connect");
        sockets.push(sock);
        monitors.push(mon);
    }

    let ready_timeout = common::resolve_multi_connect_ready_timeout();
    for monitor in &mut monitors {
        common::wait_monitor_ready(monitor, ready_timeout, "multi dealer-dealer client");
    }

    println!("CLIENT_READY,{}", args.msg_size);
    io::stdout().flush().ok();

    let stdin = io::stdin();
    let mut start_seen = false;
    for line in stdin.lock().lines() {
        let line = line.unwrap_or_default();
        if line.trim() == format!("START,{}", args.msg_size) {
            start_seen = true;
            break;
        }
        if matches!(line.trim(), "STOP" | "QUIT") {
            return;
        }
    }
    if !start_seen {
        return;
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");

    let poller = Poller::new().expect("poller");
    for (index, socket) in sockets.iter().enumerate() {
        poller
            .add_socket(socket, POLLCOMPLETION, index)
            .expect("poller add");
    }
    let mut poll_events = vec![PollEvent::default(); sockets.len().max(1)];

    let active_seconds = settings.duration_seconds;
    let latency_seconds = common::latency_seconds(active_seconds);
    let drain_ns = common::resolve_multi_send_drain_timeout().as_nanos() as u64;
    let submit = |slot: usize, message| {
        perf_submit_measurement_async!(sockets[slot].send(), message).expect("send")
    };
    let mut unused = common::LatencyStats::new();
    let (active_sends, _) = common::run_send_phase(
        &args,
        &poller,
        sockets.len(),
        drain_ns,
        common::SendPhase {
            echo: false,
            end: common::now_ns() + active_seconds * common::SECOND_NS,
            schedule: None,
        },
        submit,
        |_, _| false,
        &mut unused,
    );
    send_stop(&sockets, &poller, &mut poll_events);
    let received = common::read_phase_count("PHASE_PACED", args.msg_size);
    let mut schedule = common::Schedule::new(common::now_ns(), received, active_seconds);
    let end = schedule.epoch + common::SETTLE_NS + latency_seconds * common::SECOND_NS;
    common::run_send_phase(
        &args,
        &poller,
        sockets.len(),
        drain_ns,
        common::SendPhase {
            echo: false,
            end,
            schedule: Some(&mut schedule),
        },
        submit,
        |_, _| false,
        &mut unused,
    );
    send_stop(&sockets, &poller, &mut poll_events);
    let paced_received = common::read_phase_count("PHASE_COMPLETE", args.msg_size);
    schedule.print(
        "DEALER_DEALER",
        args.msg_size,
        (active_sends, received, active_seconds),
        paced_received,
        latency_seconds,
        None,
    );
    if let Some(socket) = sockets.first() {
        common::print_multi_auto_hwm_detail(
            socket,
            "endpoint",
            &args.transport,
            args.msg_size,
            "dealer",
        );
    }
    println!("CLIENT_DONE,{}", args.msg_size);
    io::stdout().flush().ok();
}

fn send_stop(sockets: &[DealerSocket], poller: &Poller, poll_events: &mut [PollEvent]) {
    let mut tasks = common::ConcurrentTasks::new(sockets.len());
    for (slot, socket) in sockets.iter().enumerate() {
        let submission = socket
            .send()
            .message(Message::try_from(common::STOP_TOKEN).expect("stop token"))
            .submit()
            .unwrap_or_else(|err| panic!("stop token submit failed: {err}"));
        tasks.insert(slot, submission.admitted);
    }
    while tasks.any_pending() {
        let ready = tasks.poll_ready();
        for (_, result) in ready {
            result.unwrap_or_else(|err| panic!("stop token send failed: {err}"));
        }
        if tasks.any_pending() {
            poller
                .wait(poll_events, -1)
                .unwrap_or_else(|err| panic!("poller wait failed: {err}"));
        }
    }
}
