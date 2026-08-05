//! Shared perf utilities - metric header, latency stats, phase control.

use std::fs;
use std::io;
use std::path::Path;
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use zlink::{
    Context, DealerSocket, Message, PairSocket, PubSocket, RouterSocket, SocketMonitor, SubSocket,
    SubmitError, ZlinkError,
};

// -- Metric header (29 bytes) ------------------------------------------------
// Layout matches doc/perf/PERF_POLICY.md:
//   [0..4]   magic      u32 LE  0x5A4C4E4B ("ZLNK")
//   [4..8]   run_id     u32 LE
//   [8]      phase      u8      (0=warmup, 1=active, 2=cooldown)
//   [9..13]  msg_size   u32 LE
//   [13..21] seq        u64 LE
//   [21..29] sent_ts_ns i64 LE  (nanoseconds since epoch)

// Wire-level stop token used by sender threads to signal phase end to a
// receiver waiting on a poller. PERF_SINGLE_TEST_POLICY § 1.4 mandates this
// pattern instead of `AtomicBool sender_done` + short polling.
pub const STOP_TOKEN: &[u8] = b"__zlink_perf_stop__";

pub fn is_stop_token(data: &[u8]) -> bool {
    data == STOP_TOKEN
}

pub const HEADER_SIZE: usize = 29;
pub const MAGIC: u32 = 0x5A4C_4E4B; // "ZLNK"
pub const PHASE_WARMUP: u8 = 0;
pub const PHASE_ACTIVE: u8 = 1;
pub const PHASE_COOLDOWN: u8 = 2;
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

pub fn decode_run_id(data: &[u8]) -> u32 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    u32::from_le_bytes(data[4..8].try_into().unwrap())
}

pub fn decode_msg_size(data: &[u8]) -> u32 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    u32::from_le_bytes(data[9..13].try_into().unwrap())
}

pub fn decode_sent_ts_ns(data: &[u8]) -> i64 {
    if data.len() < HEADER_SIZE {
        return 0;
    }
    i64::from_le_bytes(data[21..29].try_into().unwrap())
}

pub fn decode_phase(data: &[u8]) -> u8 {
    if data.len() < HEADER_SIZE {
        return u8::MAX;
    }
    data[8]
}

pub fn is_valid_active_message(data: &[u8], expected_size: usize) -> bool {
    is_valid_message(data, expected_size) && decode_phase(data) == PHASE_ACTIVE
}

pub fn is_valid_message(data: &[u8], expected_size: usize) -> bool {
    data.len() >= HEADER_SIZE
        && decode_magic(data) == MAGIC
        && decode_msg_size(data) as usize == expected_size
        && decode_run_id(data) == BENCHMARK_RUN_ID
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

impl_raw_tls_socket!(PairSocket, PubSocket, DealerSocket, RouterSocket, SubSocket);

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

pub fn wait_monitor_ready(mon: &mut SocketMonitor, timeout: Duration, name: &str) {
    let (tx, rx) = mpsc::sync_channel::<Result<(), String>>(1);
    mon.on_event(move |event| {
        if event.is_connection_ready() {
            let _ = tx.send(Ok(()));
        }
    })
    .unwrap_or_else(|err| panic!("{name} monitor handler install failed: {err}"));

    match rx.recv_timeout(timeout) {
        Ok(Ok(())) => {}
        Ok(Err(err)) => panic!("{name} monitor recv failed: {err}"),
        Err(mpsc::RecvTimeoutError::Timeout) => {
            panic!("{name} connection-ready wait timed out after {:?}", timeout);
        }
        Err(mpsc::RecvTimeoutError::Disconnected) => {
            panic!("{name} monitor channel disconnected before connection-ready");
        }
    }
}

pub fn poll_idle(timeout: Duration) {
    let wait_ms = timeout.as_millis().max(1) as i64;
    let _ = zlink::poll(&mut [], wait_ms);
}

pub fn perf_context() -> Context {
    let ctx = Context::new().expect("context");
    ctx.options().set_blocky(false).expect("set blocky");
    if let Ok(value) = std::env::var("PERF_IO_THREADS") {
        if let Ok(io_threads) = value.parse::<i32>() {
            if io_threads > 0 {
                ctx.options()
                    .set_io_threads(io_threads)
                    .expect("set io threads");
            }
        }
    }
    ctx
}

// -- Latency statistics ------------------------------------------------------

pub struct LatencyStats {
    samples: Vec<u64>,
    count: u64,
    sum: u64,
}

impl LatencyStats {
    pub fn new() -> Self {
        Self {
            samples: Vec::new(),
            count: 0,
            sum: 0,
        }
    }

    // C perf_single_latency.hpp latency_stats_builder_t::add(): every sample is
    // push_back'd into an unbounded growing vector; percentiles are exact.
    pub fn record_ns(&mut self, latency_ns: u64) {
        self.count += 1;
        self.sum += latency_ns;
        self.samples.push(latency_ns);
    }

    pub fn finish(&mut self) -> StatsResult {
        if self.count == 0 {
            return StatsResult::default();
        }
        self.samples.sort_unstable();
        let mean = self.sum as f64 / self.count as f64;
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

// C perf_single_latency.hpp percentile_from_sorted(): linear interpolation
// between adjacent sorted samples (exact percentile, not nearest-rank).
fn percentile(sorted: &[u64], q: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    if q <= 0.0 {
        return sorted[0] as f64;
    }
    if q >= 1.0 {
        return sorted[sorted.len() - 1] as f64;
    }
    let pos = (sorted.len() - 1) as f64 * q;
    let lo = pos as usize;
    let hi = if lo + 1 < sorted.len() { lo + 1 } else { lo };
    let frac = pos - lo as f64;
    sorted[lo] as f64 + (sorted[hi] as f64 - sorted[lo] as f64) * frac
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

pub fn build_phase_result(size: usize, duration_s: u64, stats: &StatsResult) -> PhaseResult {
    let throughput = if duration_s == 0 {
        0.0
    } else {
        stats.count as f64 / duration_s as f64
    };
    let bandwidth = throughput * size as f64 * bandwidth_multiplier("PAIR") / 1_000_000.0;

    PhaseResult {
        throughput,
        bandwidth,
        latency_mean_ns: stats.mean_ns,
        latency_p95_ns: stats.p95_ns,
        latency_p99_ns: stats.p99_ns,
    }
}

fn bandwidth_multiplier(_pattern: &str) -> f64 {
    1.0
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
    let mut phase = build_phase_result(size, duration_s, stats);
    phase.bandwidth = phase.throughput * size as f64 * bandwidth_multiplier(pattern) / 1_000_000.0;
    print_phase_result(&key, &phase);
}

pub struct MetricCollector {
    stats: Arc<Mutex<LatencyStats>>,
}

impl MetricCollector {
    pub fn new() -> Self {
        Self {
            stats: Arc::new(Mutex::new(LatencyStats::new())),
        }
    }

    pub fn shared(&self) -> Arc<Mutex<LatencyStats>> {
        Arc::clone(&self.stats)
    }

    pub fn finish(&self) -> StatsResult {
        self.stats.lock().unwrap().finish()
    }
}

/// Record active-phase latency if the payload matches the expected run.
pub fn handle_recv(
    data: &[u8],
    expected_size: usize,
    stats: &std::sync::Mutex<LatencyStats>,
    active_deadline: Instant,
) {
    if Instant::now() < active_deadline && is_valid_active_message(data, expected_size) {
        let sent_ts_ns = decode_sent_ts_ns(data);
        let latency_ns = (now_ns() as i64).saturating_sub(sent_ts_ns).max(0) as u64;
        stats.lock().unwrap().record_ns(latency_ns);
    }
}

/// Send the stop token once via the supplied closure with bounded attempts to
/// ride through transient backpressure / not-connected races.
///
/// The caller selects blocking or nonblocking submission for its socket
/// pattern. A nonblocking closure returns `Ok(false)` for transient
/// backpressure.
/// PERF_SINGLE_TEST_POLICY § 1.4 mandates this wire-level shutdown signal in
/// lieu of `AtomicBool sender_done` + short polling.
pub fn send_stop_token<F>(mut send_fn: F)
where
    F: FnMut(Message) -> Result<bool, SubmitError>,
{
    let retry_budget_ms = env_or_u64("PERF_SINGLE_STOP_SEND_RETRY_MS", 20_000);
    for _ in 0..retry_budget_ms {
        let token = Message::try_from(STOP_TOKEN).expect("stop token msg");
        match send_fn(token) {
            Ok(true) => return,
            Ok(false) => {}
            Err(err)
                if err.code() == zlink::SubmitResult::NotConnected
                    || is_single_send_retry_error(&err) => {}
            Err(err) => panic!("stop token send failed: {err}"),
        }
        poll_idle(Duration::from_millis(1));
    }
    panic!("stop token send retry exhausted");
}

// -- Send loop ---------------------------------------------------------------
// Core single perf uses blocking send in the sender thread.
// The sender must not set a send timeout – blocking is the intended behavior
// so that natural backpressure throttles the sender.

/// One-way send loop: active only.
/// `send_fn` returns false when nonblocking send cannot accept a message yet.
pub fn send_loop<S>(active_deadline: Instant, msg_size: usize, phase: u8, mut send_fn: S)
where
    S: FnMut(Message) -> bool,
{
    let mut seq: u64 = 0;
    let payload_size = msg_size.max(HEADER_SIZE);

    while Instant::now() < active_deadline {
        let mut msg = Message::with_size(payload_size).expect("msg");
        encode_header(msg.data_mut(), phase, msg_size as u32, seq);
        if send_fn(msg) {
            seq += 1;
        } else {
            continue;
        }
    }
}

// -- CLI config --------------------------------------------------------------

pub struct PerfConfig {
    pub transport: String,
    pub size: usize,
    pub duration_seconds: u64,
}

impl PerfConfig {
    pub fn from_env_and_args() -> Self {
        let args: Vec<String> = std::env::args().collect();
        let mut size = 64;
        let mut duration = 5u64;
        let mut transport = "inproc".to_string();
        let mut i = 1;

        if args.len() >= 4
            && !args[1].starts_with('-')
            && !args[2].starts_with('-')
            && !args[3].starts_with('-')
        {
            transport = args[2].clone();
            size = args[3].parse().unwrap();
            i = 4;
        }

        while i < args.len() {
            match args[i].as_str() {
                "--msg-size" if i + 1 < args.len() => {
                    size = args[i + 1].parse().unwrap();
                    i += 2;
                }
                "--duration" if i + 1 < args.len() => {
                    duration = args[i + 1].parse().unwrap();
                    i += 2;
                }
                "--transport" if i + 1 < args.len() => {
                    transport = args[i + 1].clone();
                    i += 2;
                }
                "--pattern" if i + 1 < args.len() => {
                    i += 2;
                }
                _ => {
                    i += 1;
                }
            }
        }

        assert!(
            size >= HEADER_SIZE,
            "msg-size must be >= {HEADER_SIZE}, got {size}"
        );

        Self {
            transport,
            size,
            duration_seconds: duration,
        }
    }

    pub fn endpoint(&self, suffix: &str) -> String {
        match self.transport.as_str() {
            "inproc" => format!("inproc://perf-{suffix}"),
            "ipc" => format!(
                "ipc:///tmp/zlink-rust-perf-{suffix}-{}-{}.ipc",
                std::process::id(),
                now_ns()
            ),
            "ws" => format!("ws://127.0.0.1:{}", reserve_tcp_port()),
            "wss" => format!("wss://127.0.0.1:{}", reserve_tcp_port()),
            "tls" => format!("tls://127.0.0.1:{}", reserve_tcp_port()),
            "tcp" => format!("tcp://127.0.0.1:{}", reserve_tcp_port()),
            _ => panic!("unsupported transport for endpoint: {}", self.transport),
        }
    }
}

fn env_or_u64(name: &str, default: u64) -> u64 {
    std::env::var(name)
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(default)
}

fn reserve_tcp_port() -> u16 {
    let listener = try_reserve_tcp_port().expect("reserve tcp port");
    let port = listener.local_addr().expect("tcp addr").port();
    drop(listener);
    port
}

fn try_reserve_tcp_port() -> io::Result<std::net::TcpListener> {
    std::net::TcpListener::bind("127.0.0.1:0")
}

// C bench_common_runtime.hpp bench_single_manual_socket_overrides_allowed():
// numeric SNDHWM/RCVHWM are only applied when the operator opts in via
// PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES / PERF_ALLOW_MANUAL_SOCKET_OVERRIDES.
// The default path applies NO numeric HWM (context auto-HWM governs).
fn manual_socket_overrides_allowed() -> bool {
    std::env::var("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES")
        .ok()
        .or_else(|| std::env::var("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES").ok())
        .map(|v| v == "1")
        .unwrap_or(false)
}

pub fn resolve_single_send_high_water_mark() -> u64 {
    env_or_u64("PERF_SINGLE_SNDHWM", env_or_u64("PERF_SINGLE_HWM", 0))
}

pub fn resolve_single_receive_high_water_mark() -> u64 {
    env_or_u64("PERF_SINGLE_RCVHWM", env_or_u64("PERF_SINGLE_HWM", 0))
}

// C bench_common_runtime.hpp apply_single_hwm(): gated behind the manual
// override flag; only applies positive HWM values.
pub fn apply_single_hwm<O: SingleSocketHwmOptions>(opts: &O) {
    if !manual_socket_overrides_allowed() {
        return;
    }
    let sndhwm = resolve_single_send_high_water_mark();
    let rcvhwm = resolve_single_receive_high_water_mark();
    if sndhwm > 0 {
        opts.set_send_high_water_mark(sndhwm).expect("sndhwm");
    }
    if rcvhwm > 0 {
        opts.set_receive_high_water_mark(rcvhwm).expect("rcvhwm");
    }
}

pub trait SingleSocketHwmOptions {
    fn set_send_high_water_mark(&self, hwm: u64) -> Result<(), ZlinkError>;
    fn set_receive_high_water_mark(&self, hwm: u64) -> Result<(), ZlinkError>;
}

macro_rules! impl_single_socket_hwm_options {
    ($($ty:ty),+ $(,)?) => {
        $(
            impl SingleSocketHwmOptions for $ty {
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

impl_single_socket_hwm_options!(PairSocket, PubSocket, DealerSocket, RouterSocket, SubSocket);

pub fn apply_single_auto_hwm_msg_unit(ctx: &Context, msg_size: usize) {
    if msg_size == 0 {
        return;
    }
    let unit = msg_size as u64;
    ctx.options()
        .set_auto_hwm_msg_unit_bytes(unit)
        .expect("auto hwm msg unit");
    ctx.recalculate_auto_hwm().expect("auto hwm recalculate");
}


pub fn resolve_single_idle_drain_ms() -> u64 {
    env_or_u64("PERF_SINGLE_RCVTIMEO_MS", 200)
}

pub fn resolve_single_receive_timeout() -> Duration {
    Duration::from_millis(env_or_u64("PERF_SINGLE_RCVTIMEO_MS", 200))
}

pub fn resolve_single_send_timeout() -> Duration {
    Duration::from_millis(env_or_u64("PERF_SINGLE_SNDTIMEO_MS", 200))
}

pub fn is_single_send_retry_error(err: &SubmitError) -> bool {
    matches!(
        err.native_errno(),
        libc::EAGAIN | libc::EINTR | libc::ETIMEDOUT
    )
}

pub fn resolve_single_pubsub_idle_drain_ms() -> u64 {
    env_or_u64(
        "PERF_SINGLE_PUBSUB_RCVTIMEO_MS",
        resolve_single_idle_drain_ms(),
    )
}

pub fn resolve_single_pubsub_ready_settle() -> Duration {
    Duration::from_millis(env_or_u64("PERF_SINGLE_PUBSUB_READY_SETTLE_MS", 1000))
}

pub fn resolve_single_stop_wait() -> Duration {
    Duration::from_millis(env_or_u64("PERF_SINGLE_STOP_WAIT_MS", 20_000))
}

pub fn resolve_single_ready_timeout() -> Duration {
    Duration::from_millis(env_or_u64("PERF_CONNECT_READY_TIMEOUT_MS", 1000))
}

pub fn resolve_endpoint_or_emit_unsupported(
    pattern: &str,
    transport: &str,
    suffix: &str,
) -> Option<String> {
    match transport {
        "inproc" => Some(format!("inproc://perf-{suffix}")),
        "ipc" => Some(format!(
            "ipc:///tmp/zlink-rust-perf-{suffix}-{}-{}.ipc",
            std::process::id(),
            now_ns()
        )),
        "ws" => Some(format!("ws://127.0.0.1:{}", reserve_tcp_port())),
        "wss" => Some(format!("wss://127.0.0.1:{}", reserve_tcp_port())),
        "tls" => Some(format!("tls://127.0.0.1:{}", reserve_tcp_port())),
        "tcp" => Some(format!("tcp://127.0.0.1:{}", reserve_tcp_port())),
        _ => {
            emit_unsupported(pattern, transport, "unsupported_transport");
            None
        }
    }
}
