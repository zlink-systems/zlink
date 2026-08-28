//! Multi perf common utilities.

use std::fs;
use std::future::Future;
use std::io;
use std::path::Path;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context as TaskContext, Poll, Wake, Waker};
use std::thread::{self, Thread};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use zlink::{
    AutoHwmProfile, Context, DealerSocket, Message, Monitorable, PairSocket, PollEvent, Poller,
    PubSocket, RecvFlags, RouterSocket, SocketMonitor, SocketMonitorEventMask,
    SocketMonitorOpenOptions, StreamSocket, SubSocket, ZlinkError,
};

pub const STOP_TOKEN: &[u8] = b"__zlink_perf_stop__";
pub const HEADER_SIZE: usize = 29;
pub const PHASE_WARMUP: u8 = 0;
pub const PHASE_ACTIVE: u8 = 1;
pub const PHASE_COOLDOWN: u8 = 2;
pub const MAGIC: u32 = 0x5A4C_4E4B; // "ZLNK"
pub const BENCHMARK_RUN_ID: u32 = 1;
const DEFAULT_MULTI_MONITOR_HWM_BYTES: u64 = 4_096_000;

struct ThreadWake(Thread);

impl Wake for ThreadWake {
    fn wake(self: Arc<Self>) {
        self.0.unpark();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.0.unpark();
    }
}

/// Poll a dynamically sized batch of same-type futures concurrently.
///
/// Multi perf uses this tiny executor instead of serial `block_on` calls so
/// Core completion, rather than a binding-owned window, controls admission.
pub fn block_on_all<F: Future>(futures: Vec<F>) -> Vec<F::Output> {
    let mut futures: Vec<_> = futures.into_iter().map(Box::pin).collect();
    let mut outputs = Vec::with_capacity(futures.len());
    let waker = Waker::from(Arc::new(ThreadWake(thread::current())));
    let mut context = TaskContext::from_waker(&waker);
    while !futures.is_empty() {
        let mut index = 0;
        let mut progressed = false;
        while index < futures.len() {
            match futures[index].as_mut().poll(&mut context) {
                Poll::Ready(output) => {
                    outputs.push(output);
                    drop(futures.swap_remove(index));
                    progressed = true;
                }
                Poll::Pending => index += 1,
            }
        }
        if !progressed && !futures.is_empty() {
            thread::park();
        }
    }
    outputs
}

pub struct ConcurrentTasks<F: Future> {
    tasks: Vec<Option<Pin<Box<F>>>>,
    waker: Waker,
}

impl<F: Future> ConcurrentTasks<F> {
    pub fn new(slots: usize) -> Self {
        Self {
            tasks: (0..slots).map(|_| None).collect(),
            waker: Waker::from(Arc::new(ThreadWake(thread::current()))),
        }
    }

    pub fn insert(&mut self, slot: usize, future: F) {
        assert!(self.tasks[slot].is_none(), "task slot is already occupied");
        self.tasks[slot] = Some(Box::pin(future));
    }

    pub fn push(&mut self, future: F) -> usize {
        if let Some(slot) = self.tasks.iter().position(Option::is_none) {
            self.insert(slot, future);
            slot
        } else {
            self.tasks.push(Some(Box::pin(future)));
            self.tasks.len() - 1
        }
    }

    pub fn poll_ready(&mut self) -> Vec<(usize, F::Output)> {
        let mut context = TaskContext::from_waker(&self.waker);
        let mut ready = Vec::new();
        for (slot, task) in self.tasks.iter_mut().enumerate() {
            let Some(future) = task.as_mut() else {
                continue;
            };
            if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
                *task = None;
                ready.push((slot, output));
            }
        }
        ready
    }

    pub fn is_pending(&self, slot: usize) -> bool {
        self.tasks[slot].is_some()
    }

    pub fn any_pending(&self) -> bool {
        self.tasks.iter().any(Option::is_some)
    }

    pub fn wait_for_wake(&self, timeout: Duration) {
        thread::park_timeout(timeout);
    }
}

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
    let expected = if std::env::var("PERF_PART_COUNT").ok().as_deref() == Some("1") {
        1
    } else {
        2
    };
    if parts.len() != expected || (expected == 2 && !parts[1].as_bytes().is_empty()) {
        return &[];
    }
    parts.first().map(|part| part.as_bytes()).unwrap_or(&[])
}

pub fn measurement_part_count() -> usize {
    if std::env::var("PERF_PART_COUNT").ok().as_deref() == Some("1") {
        1
    } else {
        2
    }
}

#[macro_export]
macro_rules! perf_submit_measurement_async {
    ($operation:expr, $payload:expr) => {{
        async move {
            let operation = $operation.message($payload);
            if $crate::common::measurement_part_count() == 2 {
                operation
                    .message(
                        zlink::Message::try_from(&[] as &[u8]).expect("empty measurement tail"),
                    )
                    .submit()
                    .await
            } else {
                operation.submit().await
            }
        }
    }};
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

pub fn elapsed_since_sent_ns(data: &[u8]) -> Option<f64> {
    let sent_ts_ns = decode_sent_ts_ns(data);
    if sent_ts_ns <= 0 {
        return None;
    }
    now_ns()
        .checked_sub(sent_ts_ns as u64)
        .map(|elapsed| elapsed as f64)
}

pub fn record_active_rtt_latency(
    data: &[u8],
    expected_size: usize,
    stats: &mut LatencyStats,
) -> bool {
    if !is_valid_active_message(data, expected_size) {
        return false;
    }

    stats.record_received();
    if let Some(elapsed_ns) = elapsed_since_sent_ns(data) {
        stats.record_latency_sample_ns(elapsed_ns / 2.0);
    }
    true
}

// -- Latency stats -----------------------------------------------------------

pub struct LatencyStats {
    samples: Vec<f64>,
    received_count: u64,
    latency_count: u64,
    latency_sum_ns: f64,
    sample_cap: usize,
    samples_seen: u64,
    rng: u32,
}

impl LatencyStats {
    pub fn new() -> Self {
        Self::with_sample_cap(resolve_latency_sample_cap())
    }

    fn with_sample_cap(sample_cap: usize) -> Self {
        Self {
            samples: Vec::with_capacity(sample_cap),
            received_count: 0,
            latency_count: 0,
            latency_sum_ns: 0.0,
            sample_cap,
            samples_seen: 0,
            rng: 0xA341_316C,
        }
    }

    pub fn record_ns(&mut self, ns: f64) {
        self.record_received();
        self.record_latency_sample_ns(ns);
    }

    pub fn record_received(&mut self) {
        self.received_count += 1;
    }

    pub fn record_latency_sample_ns(&mut self, ns: f64) {
        let sample = if ns >= 0.0 { ns } else { 0.0 };
        self.latency_count += 1;
        self.latency_sum_ns += sample;
        self.samples_seen += 1;
        if self.sample_cap == 0 {
            return;
        }
        if self.samples.len() < self.sample_cap {
            self.samples.push(sample);
            return;
        }

        self.rng = self.rng.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let slot = (self.rng as u64) % self.samples_seen;
        if slot < self.sample_cap as u64 {
            self.samples[slot as usize] = sample;
        }
    }

    pub fn finish(&mut self) -> StatsResult {
        self.samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let mean = if self.latency_count > 0 {
            self.latency_sum_ns / self.latency_count as f64
        } else {
            0.0
        };
        let p95 = if self.samples.is_empty() {
            mean
        } else {
            percentile(&self.samples, 0.95)
        };
        let p99 = if self.samples.is_empty() {
            mean
        } else {
            percentile(&self.samples, 0.99)
        };
        StatsResult {
            count: self.received_count,
            latency_count: self.latency_count,
            mean_ns: mean,
            p95_ns: p95,
            p99_ns: p99,
        }
    }
}

fn resolve_latency_sample_cap() -> usize {
    std::env::var("PERF_MULTI_LATENCY_SAMPLE_CAP")
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .unwrap_or(65_536)
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
    pub latency_count: u64,
    pub mean_ns: f64,
    pub p95_ns: f64,
    pub p99_ns: f64,
}

impl StatsResult {
    pub fn is_valid(&self) -> bool {
        self.count > 0 && self.latency_count > 0
    }
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
        | "MULTI_DEALER_ROUTER_SENDSEND"
        | "MULTI_DEALER_ROUTER_REQREP"
        | "MULTI_ROUTER_ROUTER"
        | "MULTI_ROUTER_ROUTER_SENDSEND"
        | "MULTI_ROUTER_ROUTER_REQREP"
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
    if !stats.is_valid() {
        eprintln!("FAIL,current,{pattern},{transport},{size}");
        std::process::exit(1);
    }
    let key = format!("RESULT,current,{pattern},{transport},{size}");
    let phase = build_phase_result(pattern, size, duration_s, stats);
    print_phase_result(&key, &phase);
}

pub fn print_ready(endpoint: &str) {
    println!("READY,{endpoint}");
    use std::io::Write;
    std::io::stdout().flush().ok();
}

pub fn open_connection_ready_monitor(socket: &dyn Monitorable) -> SocketMonitor {
    SocketMonitor::open_with_options(
        socket,
        SocketMonitorOpenOptions {
            events: SocketMonitorEventMask::CONNECTION_READY,
            monitor_hwm_bytes: resolve_multi_monitor_hwm_bytes(),
        },
    )
    .expect("connection-ready monitor")
}

pub fn wait_monitor_ready(mon: &mut SocketMonitor, timeout: Duration, name: &str) {
    // The perf clients open the monitor before connect and wait after all
    // sockets have been connected. Install-time callbacks do not replay an
    // event already queued by Core, so consume the monitor queue directly and
    // also consult the public status snapshot for an event observed before the
    // first non-blocking receive.
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        match mon.recv_with_flags(RecvFlags::DONT_WAIT) {
            Ok(Some(event)) if event.is_connection_ready() => return,
            Ok(Some(_)) | Ok(None) => {}
            Err(error) => panic!("{name} monitor receive failed: {error}"),
        }
        if let Ok(status) = mon.status() {
            if status.is_ready() {
                return;
            }
        }
        thread::yield_now();
    }
    panic!("{name} connection-ready wait timed out after {:?}", timeout);
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
    ctx.options()
        .set_auto_hwm_enabled(resolve_auto_hwm_enabled())
        .expect("set auto hwm enabled");
    ctx.options()
        .set_core_hwm_profile(resolve_auto_hwm_profile())
        .expect("set auto hwm profile");
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

fn resolve_auto_hwm_enabled() -> bool {
    std::env::var("PERF_CTX_AUTO_HWM_ENABLE")
        .ok()
        .and_then(|value| value.parse::<i32>().ok())
        .map(|value| value != 0)
        .unwrap_or(true)
}

fn auto_hwm_profile_from_name(value: &str) -> AutoHwmProfile {
    match value {
        "compact" => AutoHwmProfile::Compact,
        "low_latency" | "low-latency" => AutoHwmProfile::LowLatency,
        "throughput" => AutoHwmProfile::Throughput,
        _ => AutoHwmProfile::Balanced,
    }
}

fn resolve_auto_hwm_profile() -> AutoHwmProfile {
    let value = std::env::var("PERF_MULTI_CTX_AUTO_HWM_PROFILE")
        .ok()
        .or_else(|| std::env::var("PERF_CTX_AUTO_HWM_PROFILE").ok())
        .or_else(|| std::env::var("PERF_AUTO_HWM_PROFILE").ok())
        .unwrap_or_default();
    auto_hwm_profile_from_name(value.trim())
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
        // C/Go multi runners bind locally and normalize the resolved client
        // endpoint to loopback. Keep the same endpoint contract on Windows;
        // handing a wildcard address to a DEALER client causes reconnects.
        "tcp" => Some(format!("tcp://127.0.0.1:{port}")),
        "tls" => Some(format!("tls://127.0.0.1:{port}")),
        "ws" => Some(format!("ws://127.0.0.1:{port}")),
        "wss" => Some(format!("wss://127.0.0.1:{port}")),
        "ipc" => Some(format!(
            "ipc:///tmp/zlink-rust-perf-server-{}-{}.ipc",
            std::process::id(),
            now_ns()
        )),
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

pub fn resolve_multi_connect_ready_timeout() -> Duration {
    Duration::from_millis(env_or_multi(
        "PERF_MULTI_CONNECT_READY_TIMEOUT_MS",
        "PERF_CONNECT_READY_TIMEOUT_MS",
        1_000,
    ) as u64)
}

pub fn resolve_multi_monitor_hwm_bytes() -> u64 {
    let primary = std::env::var("PERF_MULTI_MONITOR_HWM").ok();
    let fallback = std::env::var("PERF_MONITOR_HWM").ok();
    resolve_nonnegative_u64(
        primary.as_deref(),
        fallback.as_deref(),
        DEFAULT_MULTI_MONITOR_HWM_BYTES,
    )
}

pub fn resolve_multi_reqrep_timeout() -> Duration {
    Duration::from_millis(env_or("PERF_MULTI_REQREP_TIMEOUT_MS", 200).max(1) as u64)
}

pub fn resolve_multi_reqrep_drain_timeout(request_timeout: Duration) -> Duration {
    let fallback_ms = request_timeout.as_millis().saturating_mul(4).max(1_000);
    Duration::from_millis(
        env_or_u64(
            "PERF_MULTI_REQREP_DRAIN_TIMEOUT_MS",
            fallback_ms.min(u64::MAX as u128) as u64,
        )
        .max(1),
    )
}

pub fn resolve_multi_send_drain_timeout() -> Duration {
    Duration::from_millis(env_or_u64("PERF_MULTI_SEND_DRAIN_TIMEOUT_MS", 1_000).max(1))
}

pub fn poll_timeout_until(deadline: Instant) -> i64 {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return 0;
    }
    remaining.as_millis().max(1).min(i64::MAX as u128) as i64
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

fn resolve_nonnegative_u64(primary: Option<&str>, fallback: Option<&str>, default: u64) -> u64 {
    primary
        .and_then(|value| value.parse::<u64>().ok())
        .or_else(|| fallback.and_then(|value| value.parse::<u64>().ok()))
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

#[cfg(test)]
mod tests {
    use super::*;

    fn active_payload(sent_ts_ns: i64) -> Vec<u8> {
        let mut payload = vec![0u8; 64];
        encode_header(&mut payload, PHASE_ACTIVE, 64, 1);
        payload[21..29].copy_from_slice(&sent_ts_ns.to_le_bytes());
        payload
    }

    #[test]
    fn rtt_metrics_count_active_headers_without_invalid_timestamp_samples() {
        for sent_ts_ns in [0, -1, i64::MAX] {
            let mut stats = LatencyStats::with_sample_cap(8);
            assert!(record_active_rtt_latency(
                &active_payload(sent_ts_ns),
                64,
                &mut stats,
            ));

            let result = stats.finish();
            assert_eq!(result.count, 1);
            assert_eq!(result.latency_count, 0);
            assert!(!result.is_valid());
        }
    }

    #[test]
    fn rtt_metrics_record_one_way_half_of_a_valid_round_trip() {
        let sent_ts_ns = now_ns().saturating_sub(1_000_000) as i64;
        let mut stats = LatencyStats::with_sample_cap(8);
        assert!(record_active_rtt_latency(
            &active_payload(sent_ts_ns),
            64,
            &mut stats,
        ));

        let result = stats.finish();
        assert_eq!(result.count, 1);
        assert_eq!(result.latency_count, 1);
        assert!(result.mean_ns >= 500_000.0);
        assert!(result.is_valid());
    }

    #[test]
    fn latency_reservoir_bounds_percentile_memory_but_keeps_exact_mean() {
        let mut stats = LatencyStats::with_sample_cap(4);
        for sample in 0..100 {
            stats.record_ns(sample as f64);
        }
        assert_eq!(stats.samples.len(), 4);

        let result = stats.finish();
        assert_eq!(result.count, 100);
        assert_eq!(result.latency_count, 100);
        assert_eq!(result.mean_ns, 49.5);
        assert!(result.is_valid());
    }

    #[test]
    fn zero_sample_cap_falls_back_to_exact_mean_for_percentiles() {
        let mut stats = LatencyStats::with_sample_cap(0);
        stats.record_ns(10.0);
        stats.record_ns(30.0);

        let result = stats.finish();
        assert_eq!(result.mean_ns, 20.0);
        assert_eq!(result.p95_ns, 20.0);
        assert_eq!(result.p99_ns, 20.0);
        assert!(result.is_valid());
    }

    #[test]
    fn canonical_sendsend_patterns_use_round_trip_bandwidth() {
        let stats = StatsResult {
            count: 10,
            latency_count: 10,
            mean_ns: 1.0,
            p95_ns: 1.0,
            p99_ns: 1.0,
        };
        for pattern in [
            "MULTI_DEALER_ROUTER_SENDSEND",
            "MULTI_ROUTER_ROUTER_SENDSEND",
        ] {
            let result = build_phase_result(pattern, 100, 2, &stats);
            assert_eq!(result.throughput, 5.0);
            assert_eq!(result.bandwidth, 0.001);
        }
    }

    #[test]
    fn percentile_uses_linear_interpolation() {
        assert_eq!(percentile(&[0.0, 100.0], 0.95), 95.0);
    }

    #[test]
    fn auto_hwm_profile_names_match_perf_policy() {
        assert_eq!(
            auto_hwm_profile_from_name("compact"),
            AutoHwmProfile::Compact
        );
        assert_eq!(
            auto_hwm_profile_from_name("low_latency"),
            AutoHwmProfile::LowLatency
        );
        assert_eq!(
            auto_hwm_profile_from_name("low-latency"),
            AutoHwmProfile::LowLatency
        );
        assert_eq!(
            auto_hwm_profile_from_name("throughput"),
            AutoHwmProfile::Throughput
        );
        assert_eq!(
            auto_hwm_profile_from_name("unknown"),
            AutoHwmProfile::Balanced
        );
    }

    #[test]
    fn monitor_hwm_is_nonnegative_and_has_exact_multi_default() {
        assert_eq!(
            resolve_nonnegative_u64(None, None, DEFAULT_MULTI_MONITOR_HWM_BYTES),
            4_096_000
        );
        assert_eq!(
            resolve_nonnegative_u64(Some("12345"), Some("67890"), 1),
            12_345
        );
        assert_eq!(resolve_nonnegative_u64(Some("0"), None, 1), 0);
        assert_eq!(
            resolve_nonnegative_u64(Some("-1"), Some("67890"), 1),
            67_890
        );
        assert_eq!(resolve_nonnegative_u64(Some("invalid"), None, 99), 99);
    }
}
