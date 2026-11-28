#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead};
use std::time::Duration;
use zlink::Message;

const TOPIC: &str = "bench";

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_server_context();
    let pub_sock = ctx.create_pub_socket().expect("pub");
    common::apply_multi_hwm(&pub_sock, &settings);
    pub_sock
        .common_options()
        .set_send_timeout(common::timeout_ms(settings.send_timeout_ms))
        .expect("sndtimeo");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&pub_sock, &tls).expect("server tls");
    }
    let Some(bind_endpoint) = common::resolve_server_bind_endpoint("PUBSUB", &args.transport)
    else {
        return;
    };
    if let Err(err) = pub_sock.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("PUBSUB", &args.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = pub_sock.last_endpoint().expect("endpoint");
    common::print_ready(&endpoint);

    if !common::wait_for_start_stdin(args.msg_size) {
        return;
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");

    let active_seconds = settings.duration_seconds;
    let latency_seconds = common::latency_seconds(active_seconds);
    let msg_size = args.msg_size;
    let payload_size = msg_size.max(common::HEADER_SIZE);
    let mut seq = 0u64;
    let publish = |msg| {
        if common::measurement_part_count() == 2 {
            pub_sock
                .publish(TOPIC)
                .message(msg)
                .message(Message::try_from(&[] as &[u8]).expect("empty measurement tail"))
                .submit()
        } else {
            pub_sock.publish(TOPIC).message(msg).submit()
        }
        .expect("publish");
    };
    let mut send = |phase| {
        seq += 1;
        let mut msg = Message::with_size(payload_size).expect("message");
        common::encode_header(msg.data_mut(), phase, msg_size as u32, seq);
        publish(msg);
    };
    let stop = |phase| {
        let mut marker = Message::with_size(common::HEADER_SIZE).expect("marker");
        common::encode_header_at(marker.data_mut(), phase, msg_size as u32, 0, 0);
        publish(marker);
        println!("PHASE_DONE,{msg_size},{phase}");
    };
    let deadline = common::now_ns() + active_seconds * common::SECOND_NS;
    let mut active_sends = 0;
    while common::now_ns() < deadline {
        send(common::PHASE_ACTIVE);
        active_sends += u64::from(common::now_ns() < deadline);
    }
    common::print_multi_auto_hwm_detail(&pub_sock, "endpoint", &args.transport, msg_size, "pub");
    stop(common::PHASE_ACTIVE);
    let received = common::read_phase_count("PHASE_PACED", msg_size);
    let (control_tx, control_rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        for line in io::stdin().lock().lines() {
            let line = line.expect("phase control read");
            if line == format!("PHASE_CATCHUP,{msg_size}") {
                if control_tx.send((common::now_ns(), None)).is_err() {
                    return;
                }
            } else if let Some(count) = line.strip_prefix(&format!("PHASE_COMPLETE,{msg_size},")) {
                let _ = control_tx.send((
                    common::now_ns(),
                    Some(count.parse::<u64>().expect("phase count")),
                ));
                return;
            } else if matches!(line.trim(), "STOP" | "QUIT") {
                return;
            }
        }
    });
    let mut schedule = common::Schedule::new(common::now_ns(), received, active_seconds);
    let catch_up_deadline = schedule.epoch + latency_seconds * common::SECOND_NS;
    let mut end = catch_up_deadline + latency_seconds * common::SECOND_NS;
    let mut catch_up = None;
    loop {
        if catch_up.is_none() {
            if let Ok((when, None)) = control_rx.try_recv() {
                if when <= catch_up_deadline {
                    end = when + latency_seconds * common::SECOND_NS;
                    catch_up = Some((when - schedule.epoch) as f64 / common::SECOND_NS as f64);
                }
            }
        }
        let now = common::now_ns();
        if now >= end {
            break;
        }
        if schedule.due(now) {
            let due = schedule.reserve();
            send(if catch_up.is_some() {
                common::PHASE_LATENCY
            } else {
                common::PHASE_WARMUP
            });
            schedule.admit(due, common::now_ns());
        } else {
            let ms = common::wait_ms(now, end.min(schedule.next()));
            if ms > 0 {
                std::thread::sleep(Duration::from_millis(ms as u64));
            }
        }
    }
    stop(common::PHASE_LATENCY);
    let paced_received = loop {
        let (_, count) = control_rx
            .recv_timeout(Duration::from_secs(10))
            .expect("phase completion");
        if let Some(count) = count {
            break count;
        }
    };
    schedule.print(
        "PUBSUB",
        msg_size,
        (active_sends, received, active_seconds),
        paced_received,
        latency_seconds,
        catch_up,
    );
    if schedule.late > 0 {
        println!(
            "REFERENCE,current,PUBSUB,{},{msg_size},sender_late,{}",
            args.transport, schedule.late
        );
    }
}
