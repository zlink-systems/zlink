//! Multi perf common utilities.

use std::fs;
use std::io;
use std::path::Path;
use std::sync::mpsc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use zlink::{
    Context, DealerSocket, Message, PairSocket, PollEvent, Poller, PubSocket, RouterSocket,
    SocketMonitor, StreamSocket, SubSocket, ZlinkError,
};

pub const STOP_TOKEN: &[u8] = b"__zlink_perf_stop__";
pub const HEADER_SIZE: usize = 29;
pub const PHASE_WARMUP: u8 = 0;
pub const PHASE_ACTIVE: u8 = 1;
pub const PHASE_COOLDOWN: u8 = 2;
pub const MAGIC: u32 = 0x5A4C_4E4B; // "ZLNK"
pub const BENCHMARK_RUN_ID: u32 = 1;

pub fn encode_header(buf: &mut [u8], phase: u8, msg_size: u32, seq: u64) {
    buf[0..4].copy_from_slice(&MAGIC.to_le_bytes());
    buf[4..8].copy_from_slice(&BENCHMARK_RUN_ID.to_le_bytes());
    buf[8] = phase;
    buf[9..13].copy_from_slice(&msg_size.to_le_bytes());
    buf[13..21].copy_from_slice(&seq.to_le_bytes());
    buf[21..29].copy_from_slice(&(now_ns() as i64).to_le_bytes());
}

pub fn decode_magic(data: &[u8]) -> u32 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    u32::from_le_bytes(data[0..4].try_into().unwrap())
}

pub fn decode_phase(data: &[u8]) -> u8 {
    if data.len() < HEADER_SIZE {
        return u8::MAX;
    }
    data[8]
}

pub fn decode_msg_size(data: &[u8]) -> u32 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    u32::from_le_bytes(data[9..13].try_into().unwrap())
}

pub fn decode_run_id(data: &[u8]) -> u32 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    u32::from_le_bytes(data[4..8].try_into().unwrap())
}

pub fn decode_sent_ts_ns(data: &[u8]) -> i64 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    i64::from_le_bytes(data[21..29].try_into().unwrap())
}

pub fn message_payload<'a>(parts: &'a [Message]) -> &'a [u8] {
    parts.last().map(|part| part.as_bytes()).unwrap_or(&[])
}

pub fn now_ns() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos() as u64
}

pub struct TlsPaths {
    pub cert: String,
    pub key: String,
    pub ca: String,
}

pub struct TlsPem {
    pub cert: String,
    pub key: String,
    pub ca: String,
}

pub trait RawTlsSocket {
    fn set_tls_cert(&self, cert: &str) -> Result<(), ZlinkError>;
    fn set_tls_key(&self, key: &str) -> Result<(), ZlinkError>;
    fn set_tls_ca(&self, ca: &str) -> Result<(), ZlinkError>;
    fn set_tls_hostname(&self, hostname: &str) -> Result<(), ZlinkError>;
    fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ZlinkError>;
}

macro_rules! impl_raw_tls_socket {
    ($($ty:ty),+ $(,)?) => {
        $(
            impl RawTlsSocket for $ty {
                fn set_tls_cert(&self, cert: &str) -> Result<(), ZlinkError> {
                    Ok(<$ty>::set_tls_cert(self, cert)?)
                }
                fn set_tls_key(&self, key: &str) -> Result<(), ZlinkError> {
                    Ok(<$ty>::set_tls_key(self, key)?)
                }
                fn set_tls_ca(&self, ca: &str) -> Result<(), ZlinkError> {
                    Ok(<$ty>::set_tls_ca(self, ca)?)
                }
                fn set_tls_hostname(&self, hostname: &str) -> Result<(), ZlinkError> {
                    Ok(<$ty>::set_tls_hostname(self, hostname)?)
                }
                fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ZlinkError> {
                    Ok(<$ty>::set_tls_trust_system(self, trust_system)?)
                }
            }
        )+
    };
}

impl_raw_tls_socket!(
    PairSocket,
    PubSocket,
    DealerSocket,
    RouterSocket,
    StreamSocket,
    SubSocket
);

fn resolve_perf_tls_paths_from(start: &Path) -> Option<TlsPaths> {
    let mut cur = if start.is_file() {
        start.parent()?.to_path_buf()
    } else {
        start.to_path_buf()
    };

    loop {
        for candidate in [
            cur.join("bindings")
                .join("cpp")
                .join("tests")
                .join("certs")
                .join("gen"),
            cur.join("bindings")
                .join("rust")
                .join("tests")
                .join("certs")
                .join("gen"),
            cur.join("bindings")
                .join("java")
                .join("tests")
                .join("certs"),
            cur.join("bindings")
                .join("dotnet")
                .join("tests")
                .join("certs"),
            cur.join("tests").join("certs").join("gen"),
        ] {
            if candidate.join("server.crt").is_file()
                && candidate.join("server.key").is_file()
                && candidate.join("ca.crt").is_file()
            {
                return Some(TlsPaths {
                    cert: candidate.join("server.crt").to_string_lossy().into_owned(),
                    key: candidate.join("server.key").to_string_lossy().into_owned(),
                    ca: candidate.join("ca.crt").to_string_lossy().into_owned(),
                });
            }
        }

        let parent = match cur.parent() {
            Some(parent) => parent.to_path_buf(),
            None => break,
        };
        if parent == cur {
            break;
        }
        cur = parent;
    }

    None
}

pub fn resolve_perf_tls_paths() -> Option<TlsPaths> {
    if let Ok(cwd) = std::env::current_dir() {
        if let Some(paths) = resolve_perf_tls_paths_from(&cwd) {
            return Some(paths);
        }
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(paths) = resolve_perf_tls_paths_from(&exe) {
            return Some(paths);
        }
    }

    None
}

pub fn setup_raw_tls_server<S: RawTlsSocket>(socket: &S, tls: &TlsPaths) -> Result<(), ZlinkError> {
    socket.set_tls_cert(&tls.cert)?;
    socket.set_tls_key(&tls.key)?;
    Ok(())
}

pub fn setup_raw_tls_client<S: RawTlsSocket>(socket: &S, tls: &TlsPaths) -> Result<(), ZlinkError> {
    socket.set_tls_ca(&tls.ca)?;
    socket.set_tls_hostname("localhost")?;
    socket.set_tls_trust_system(false)?;
    Ok(())
}

pub fn load_tls_pem(tls: &TlsPaths) -> TlsPem {
    TlsPem {
        cert: fs::read_to_string(&tls.cert).expect("read tls cert"),
        key: fs::read_to_string(&tls.key).expect("read tls key"),
        ca: fs::read_to_string(&tls.ca).expect("read tls ca"),
    }
}

pub fn emit_unsupported(pattern: &str, transport: &str, reason: &str) {
    let _ = reason;
    println!("UNSUPPORTED,rust,{pattern},{transport}");
    use std::io::Write;
    std::io::stdout().flush().ok();
}

pub fn is_transport_unsupported_error(err: &ZlinkError) -> bool {
    matches!(
        err.native_errno(),
        libc::EPERM | libc::EACCES | libc::ENOTSUP
    )
}

pub fn handle_transport_setup_error<E>(pattern: &str, transport: &str, stage: &str, err: E) -> bool
where
    E: Into<ZlinkError> + Copy,
{
    let err = err.into();
    if is_transport_unsupported_error(&err) {
        emit_unsupported(
            pattern,
            transport,
            &format!("{stage}_errno_{}", err.native_errno()),
        );
        return true;
    }
    false
}

pub fn is_stop_token(data: &[u8]) -> bool {
    data == STOP_TOKEN
}

pub fn is_valid_message(data: &[u8], expected_size: usize) -> bool {
    data.len() >= HEADER_SIZE
        && decode_magic(data) == MAGIC
        && decode_run_id(data) == BENCHMARK_RUN_ID
        && decode_msg_size(data) as usize == expected_size
}

pub fn is_valid_active_message(data: &[u8], expected_size: usize) -> bool {
    is_valid_message(data, expected_size) && decode_phase(data) == PHASE_ACTIVE
}

// -- Latency stats -----------------------------------------------------------

pub struct LatencyStats {
    samples: Vec<f64>,
    count: u64,
    sum: f64,
}

impl LatencyStats {
    pub fn new() -> Self {
        Self {
            samples: Vec::new(),
            count: 0,
            sum: 0.0,
        }
    }

    // C perf_multi_metrics.hpp bench_latency_sampler_t: every sample retained in
    // an unbounded growing vector; percentiles are exact.
    pub fn record_ns(&mut self, ns: f64) {
        self.count += 1;
        self.sum += ns;
        self.samples.push(ns);
    }

    pub fn record_received(&mut self) {
        self.count += 1;
    }

    pub fn record_latency_sample_ns(&mut self, ns: f64) {
        self.sum += ns;
        self.samples.push(ns);
    }

    /// Fold another sampler's totals in (used to combine per-worker drain
    /// threads into a single result before finish()).
    pub fn merge(&mut self, mut other: LatencyStats) {
        self.count += other.count;
        self.sum += other.sum;
        self.samples.append(&mut other.samples);
    }

    pub fn finish(&mut self) -> StatsResult {
        if self.count == 0 {
            return StatsResult::default();
        }
        self.samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let sample_count = self.samples.len() as f64;
        let mean = if sample_count > 0.0 {
            self.sum / sample_count
        } else {
            0.0
        };
        let p95 = percentile(&self.samples, 0.95);
        let p99 = percentile(&self.samples, 0.99);
        StatsResult {
            count: self.count,
            mean_ns: mean,
            p95_ns: p95,
            p99_ns: p99,
        }
    }
}

// C perf_multi_metrics.hpp percentile_from_sorted(): linear interpolation
// between adjacent sorted samples (exact percentile, not nearest-rank).
fn percentile(sorted: &[f64], q: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    if q <= 0.0 {
        return sorted[0];
    }
    if q >= 1.0 {
        return sorted[sorted.len() - 1];
    }
    let pos = (sorted.len() - 1) as f64 * q;
    let lo = pos as usize;
    let hi = if lo + 1 < sorted.len() { lo + 1 } else { lo };
    let frac = pos - lo as f64;
    sorted[lo] + (sorted[hi] - sorted[lo]) * frac
}

#[derive(Default)]
pub struct StatsResult {
    pub count: u64,
    pub mean_ns: f64,
    pub p95_ns: f64,
    pub p99_ns: f64,
}

#[derive(Default)]
pub struct PhaseResult {
    pub throughput: f64,
    pub bandwidth: f64,
    pub latency_mean_ns: f64,
    pub latency_p95_ns: f64,
    pub latency_p99_ns: f64,
}

fn bandwidth_multiplier(pattern: &str) -> f64 {
    match pattern {
        "MULTI_DEALER_ROUTER"
        | "MULTI_ROUTER_ROUTER"
        | "MULTI_STREAM" => 2.0,
        _ => 1.0,
    }
}

pub fn build_phase_result(
    pattern: &str,
    size: usize,
    duration_s: u64,
    stats: &StatsResult,
) -> PhaseResult {
    let throughput = if duration_s == 0 {
        0.0
    } else {
        stats.count as f64 / duration_s as f64
    };
    let bandwidth = throughput * size as f64 * bandwidth_multiplier(pattern) / 1_000_000.0;

    PhaseResult {
        throughput,
        bandwidth,
        latency_mean_ns: stats.mean_ns,
        latency_p95_ns: stats.p95_ns,
        latency_p99_ns: stats.p99_ns,
    }
}

// -- RESULT output -----------------------------------------------------------

pub fn print_phase_result(key: &str, phase: &PhaseResult) {
    println!("{key},throughput,{:.3}", phase.throughput);
    println!("{key},bandwidth,{:.3}", phase.bandwidth);
    println!("{key},latency,{:.3}", phase.latency_mean_ns / 1_000_000.0);
    println!(
        "{key},latency_p95,{:.3}",
        phase.latency_p95_ns / 1_000_000.0
    );
    println!(
        "{key},latency_p99,{:.3}",
        phase.latency_p99_ns / 1_000_000.0
    );
    use std::io::Write;
    std::io::stdout().flush().ok();
}

pub fn print_result(
    pattern: &str,
    transport: &str,
    size: usize,
    duration_s: u64,
    stats: &StatsResult,
) {
    let key = format!("RESULT,current,{pattern},{transport},{size}");
    let phase = build_phase_result(pattern, size, duration_s, stats);
    print_phase_result(&key, &phase);
}

pub fn print_ready(endpoint: &str) {
    println!("READY,{endpoint}");
    use std::io::Write;
    std::io::stdout().flush().ok();
}

pub fn wait_monitor_ready(mon: &mut SocketMonitor, timeout: Duration, name: &str) {
    let (tx, rx) = mpsc::sync_channel::<()>(1);
    mon.on_event(move |event| {
        if event.is_connection_ready() {
            let _ = tx.send(());
        }
    })
    .unwrap_or_else(|err| panic!("{name} monitor handler install failed: {err}"));

    match rx.recv_timeout(timeout) {
        Ok(()) => {}
        Err(mpsc::RecvTimeoutError::Timeout) => {
            panic!("{name} connection-ready wait timed out after {:?}", timeout);
        }
        Err(mpsc::RecvTimeoutError::Disconnected) => {
            panic!("{name} monitor channel disconnected before connection-ready");
        }
    }
}

pub fn poll_idle_until(deadline: Instant, max_wait: Duration) {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return;
    }
    let wait_ms = remaining.min(max_wait).as_millis().max(1) as i64;
    let _ = zlink::poll(&mut [], wait_ms);
}

pub fn wait_control_readable_until(poller: &Poller, events: &mut [PollEvent], deadline: Instant) {
    wait_poller_until(poller, events, deadline, Duration::from_millis(50));
}

fn wait_poller_until(
    poller: &Poller,
    events: &mut [PollEvent],
    deadline: Instant,
    max_wait: Duration,
) {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return;
    }
    let wait_ms = remaining.min(max_wait).as_millis().max(1) as i64;
    if let Err(err) = poller.wait(events, wait_ms) {
        panic!("perf poller wait failed: {err}");
    }
}

fn perf_context_with_env(primary_env: &str) -> Context {
    let ctx = Context::new().expect("context");
    ctx.options().set_blocky(false).expect("set blocky");
    let io_threads = std::env::var(primary_env)
        .ok()
        .or_else(|| std::env::var("PERF_IO_THREADS").ok())
        .or_else(|| std::env::var("PERF_MULTI_DEFAULT_IO_THREADS").ok())
        .or_else(|| std::env::var("PERF_DEFAULT_IO_THREADS").ok())
        .and_then(|value| value.parse::<i32>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(4);
    ctx.options()
        .set_io_threads(io_threads)
        .expect("set io threads");
    ctx
}

pub fn perf_server_context() -> Context {
    perf_context_with_env("PERF_MULTI_SERVER_IO_THREADS")
}

pub fn perf_client_context() -> Context {
    perf_context_with_env("PERF_MULTI_CLIENT_IO_THREADS")
}

pub fn resolve_server_bind_endpoint(pattern: &str, transport: &str) -> Option<String> {
    let port = std::env::var("PERF_MULTI_SERVER_BIND_PORT")
        .ok()
        .and_then(|value| value.parse::<u16>().ok())
        .unwrap_or(0);
    match transport {
        "tcp" => Some(format!("tcp://0.0.0.0:{port}")),
        "tls" => Some(format!("tls://0.0.0.0:{port}")),
        "ws" => Some(format!("ws://0.0.0.0:{port}")),
        "wss" => Some(format!("wss://0.0.0.0:{port}")),
        _ => {
            emit_unsupported(pattern, transport, "unsupported_transport");
            None
        }
    }
}

pub fn benchmark_endpoint(pattern: &str, transport: &str, suffix: &str) -> Option<String> {
    match transport {
        "tcp" => Some(format!("tcp://127.0.0.1:{}", reserve_tcp_port())),
        "tls" => Some(format!("tls://127.0.0.1:{}", reserve_tcp_port())),
        "ws" => Some(format!("ws://127.0.0.1:{}", reserve_tcp_port())),
        "wss" => Some(format!("wss://127.0.0.1:{}", reserve_tcp_port())),
        "ipc" => Some(format!(
            "ipc:///tmp/zlink-rust-perf-{suffix}-{}-{}.ipc",
            std::process::id(),
            now_ns()
        )),
        _ => {
            emit_unsupported(pattern, transport, "unsupported_transport");
            None
        }
    }
}

fn reserve_tcp_port() -> u16 {
    let listener = try_reserve_tcp_port().expect("reserve tcp port");
    listener.local_addr().expect("reserved addr").port()
}

fn try_reserve_tcp_port() -> io::Result<std::net::TcpListener> {
    std::net::TcpListener::bind(("127.0.0.1", 0))
}

// -- Settings from env -------------------------------------------------------

pub struct MultiSettings {
    pub clients: usize,
    pub duration_seconds: u64,
    pub send_high_water_mark: u64,
    pub receive_high_water_mark: u64,
    pub send_timeout_ms: u64,
    pub receive_timeout_ms: u64,
}

impl MultiSettings {
    pub fn from_env() -> Self {
        Self {
            clients: env_or("PERF_MULTI_CLIENTS", 100),
            duration_seconds: env_or("PERF_MULTI_DURATION_SECONDS", 5) as u64,
            send_high_water_mark: env_or_u64("PERF_MULTI_SNDHWM", env_or_u64("PERF_MULTI_HWM", 0)),
            receive_high_water_mark: env_or_u64(
                "PERF_MULTI_RCVHWM",
                env_or_u64("PERF_MULTI_HWM", 0),
            ),
            send_timeout_ms: env_or("PERF_MULTI_SNDTIMEO_MS", 200) as u64,
            receive_timeout_ms: env_or("PERF_MULTI_RCVTIMEO_MS", 200) as u64,
        }
    }
}

// C perf_multi_runtime.hpp bench_manual_socket_overrides_allowed(): numeric
// SNDHWM/RCVHWM only applied when the operator opts in; default path applies
// no numeric HWM (context auto-HWM governs).
fn manual_socket_overrides_allowed() -> bool {
    std::env::var("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES")
        .ok()
        .or_else(|| std::env::var("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES").ok())
        .map(|v| v == "1")
        .unwrap_or(false)
}

pub trait MultiSocketHwmOptions {
    fn set_send_high_water_mark(&self, hwm: u64) -> Result<(), ZlinkError>;
    fn set_receive_high_water_mark(&self, hwm: u64) -> Result<(), ZlinkError>;
}

macro_rules! impl_multi_socket_hwm_options {
    ($($ty:ty),+ $(,)?) => {
        $(
            impl MultiSocketHwmOptions for $ty {
                fn set_send_high_water_mark(&self, hwm: u64) -> Result<(), ZlinkError> {
                    Ok(self.common_options().set_send_high_water_mark(hwm)?)
                }
                fn set_receive_high_water_mark(&self, hwm: u64) -> Result<(), ZlinkError> {
                    Ok(self.common_options().set_receive_high_water_mark(hwm)?)
                }
            }
        )+
    };
}

impl_multi_socket_hwm_options!(
    PairSocket,
    PubSocket,
    DealerSocket,
    RouterSocket,
    StreamSocket,
    SubSocket
);

// C perf_multi_runtime.hpp apply_benchmark_hwm(): gated behind manual-override
// flag; only applies positive HWM values.
pub fn apply_multi_hwm<O: MultiSocketHwmOptions>(opts: &O, settings: &MultiSettings) {
    if !manual_socket_overrides_allowed() {
        return;
    }
    if settings.send_high_water_mark > 0 {
        opts.set_send_high_water_mark(settings.send_high_water_mark)
            .expect("sndhwm");
    }
    if settings.receive_high_water_mark > 0 {
        opts.set_receive_high_water_mark(settings.receive_high_water_mark)
            .expect("rcvhwm");
    }
}

pub fn apply_multi_auto_hwm_msg_unit(ctx: &Context, msg_size: usize) {
    if msg_size == 0 {
        return;
    }
    let unit = msg_size as u64;
    ctx.options()
        .set_auto_hwm_msg_unit_bytes(unit)
        .expect("auto hwm msg unit");
    ctx.recalculate_auto_hwm().expect("auto hwm recalculate");
}

pub fn resolve_multi_connect_ready_timeout() -> Duration {
    Duration::from_millis(env_or_multi(
        "PERF_MULTI_CONNECT_READY_TIMEOUT_MS",
        "PERF_CONNECT_READY_TIMEOUT_MS",
        1_000,
    ) as u64)
}

fn env_or_multi(primary: &str, fallback_name: &str, default: usize) -> usize {
    std::env::var(primary)
        .ok()
        .or_else(|| std::env::var(fallback_name).ok())
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn env_or(name: &str, default: usize) -> usize {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn env_or_u64(name: &str, default: u64) -> u64 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

// -- CLI parsing for server/client binaries ----------------------------------

pub struct MultiArgs {
    pub transport: String,
    pub msg_size: usize,
    pub endpoint: String, // client only
}

impl MultiArgs {
    pub fn parse() -> Self {
        let args: Vec<String> = std::env::args().collect();
        let mut transport = "tcp".to_string();
        let mut msg_size: usize = 64;
        let mut endpoint = String::new();
        let mut index = 1;

        if args.len() >= 4
            && !args[1].starts_with('-')
            && !args[2].starts_with('-')
            && !args[3].starts_with('-')
        {
            if args.len() >= 5 && !args[4].starts_with('-') {
                transport = args[2].clone();
                msg_size = args[3].parse().unwrap();
                endpoint = args[4].clone();
                index = 5;
            } else {
                transport = args[1].clone();
                msg_size = args[2].parse().unwrap();
                endpoint = args[3].clone();
                index = 4;
            }
        } else if args.len() >= 3 && !args[1].starts_with('-') && !args[2].starts_with('-') {
            transport = args[1].clone();
            msg_size = args[2].parse().unwrap();
            index = 3;
        }

        while index < args.len() {
            match args[index].as_str() {
                "--transport" if index + 1 < args.len() => {
                    transport = args[index + 1].clone();
                    index += 2;
                }
                "--msg-size" if index + 1 < args.len() => {
                    msg_size = args[index + 1].parse().unwrap();
                    index += 2;
                }
                "--endpoint" if index + 1 < args.len() => {
                    endpoint = args[index + 1].clone();
                    index += 2;
                }
                _ => {
                    index += 1;
                }
            }
        }

        Self {
            transport,
            msg_size,
            endpoint,
        }
    }
}

// -- Wait for stdin STOP (server-side) ---------------------------------------

pub fn wait_for_stop_stdin() {
    use std::io::BufRead;
    let stdin = std::io::stdin();
    for line in stdin.lock().lines() {
        match line {
            Ok(l) if l.trim() == "STOP" || l.trim() == "QUIT" => break,
            Err(_) => break,
            _ => {}
        }
    }
}
