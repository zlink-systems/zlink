use std::io::{self, BufRead};

// PERF_MULTI_TEST_POLICY §5.1 uses the same limit for sender and reply turns.
pub const MAX_BURST_PER_SENDER: usize = 64;

#[derive(Default)]
pub struct SenderRotation {
    pub slot: usize,
    burst: usize,
}

impl SenderRotation {
    pub fn can_submit(&self) -> bool {
        self.burst < MAX_BURST_PER_SENDER
    }

    pub fn submitted(&mut self) {
        self.burst += 1;
    }

    pub fn advance(&mut self, senders: usize) {
        self.slot = (self.slot + 1) % senders;
        self.burst = 0;
    }
}

pub fn reply_turn(replies: &mut usize) -> bool {
    *replies += 1;
    if *replies < MAX_BURST_PER_SENDER {
        return false;
    }
    *replies = 0;
    true
}

pub const SETTLE_NS: u64 = 500_000_000;
pub const SECOND_NS: u64 = 1_000_000_000;
const RECEIVE_QUANTUM: u64 = 1;

pub fn wait_ms(now: u64, until: u64) -> i64 {
    (until.saturating_sub(now) / 1_000_000).min(50) as i64
}

pub fn active_receive(received: u64, deadline: u64) -> bool {
    received < deadline
}

pub fn latency_seconds(active_seconds: u64) -> u64 {
    std::env::var("PERF_MULTI_LATENCY_DURATION_SECONDS")
        .ok()
        .and_then(|s| s.parse::<u64>().ok())
        .filter(|s| *s > 0)
        .unwrap_or(active_seconds)
}

// C diagnostics use defaultfloat with a fixed number of significant digits.
pub fn diagnostic_number(value: f64, precision: usize) -> String {
    if !value.is_finite() {
        return value.to_string();
    }
    let digits = precision - 1;
    let scientific = format!("{value:.digits$e}");
    let (coefficient, exponent) = scientific.split_once('e').expect("scientific notation");
    let exponent: i32 = exponent.parse().expect("decimal exponent");
    fn trim_fraction(text: &str) -> &str {
        if text.contains('.') {
            text.trim_end_matches('0').trim_end_matches('.')
        } else {
            text
        }
    }
    if (-4..precision as i32).contains(&exponent) {
        let decimals = (digits as i32 - exponent) as usize;
        let fixed = format!("{value:.decimals$}");
        trim_fraction(&fixed).to_owned()
    } else {
        format!("{}e{exponent:+03}", trim_fraction(coefficient))
    }
}

pub struct Schedule {
    pub epoch: u64,
    pub rate: f64,
    pub accepted: u64,
    pub late: u64,
    reserved: u64,
}

impl Schedule {
    pub fn new(epoch: u64, received: u64, seconds: u64) -> Self {
        Self {
            epoch,
            rate: received as f64 / seconds as f64 * 0.8,
            accepted: 0,
            late: 0,
            reserved: 0,
        }
    }

    pub fn next(&self) -> u64 {
        self.epoch + (self.reserved as f64 * SECOND_NS as f64 / self.rate) as u64
    }

    pub fn due(&self, now: u64) -> bool {
        self.rate > 0.0 && now >= self.next()
    }

    pub fn reserve(&mut self) -> u64 {
        let due = self.next();
        self.reserved += 1;
        due
    }

    pub fn admit(&mut self, due: u64, now: u64) {
        self.accepted += 1;
        self.late += u64::from(now.saturating_sub(due) > 1_000_000);
    }

    pub fn scheduled(&self, elapsed: u64) -> u64 {
        (self.rate * (elapsed as f64 / SECOND_NS as f64)) as u64
    }

    pub fn print(
        &self,
        pattern: &str,
        size: usize,
        active: (u64, u64, u64),
        paced_received: u64,
        latency_seconds: u64,
        catch_up: Option<f64>,
    ) {
        let (active_sends, active_received, active_seconds) = active;
        let decimal = |value| diagnostic_number(value, 12);
        println!(
            "BENCH_DIAG,{pattern},{size},active_sender_rate,{},target_sender_rate,{},latency_sends,{},late_sends_gt_1ms,{},received_rate,{},paced_received_rate,{},caught_up,{},catch_up_seconds,{}",
            decimal(active_sends as f64 / active_seconds as f64),
            decimal(self.rate),
            self.accepted,
            self.late,
            decimal(active_received as f64 / active_seconds as f64),
            decimal(paced_received as f64 / latency_seconds as f64),
            if catch_up.is_some() { "yes" } else { "no" },
            decimal(catch_up.unwrap_or(-1.0)),
        );
    }
}

#[derive(Default)]
pub struct CatchUp {
    received: Vec<u64>,
    scheduled: u64,
    start_ns: u64,
    pub start_in_flight: u64,
    pub in_flight: u64,
}

impl CatchUp {
    pub fn observe(&mut self, now: u64, received: &[u64], scheduled: u64) -> bool {
        let Some(minimum) = received.iter().min() else {
            return false;
        };
        self.in_flight = scheduled.saturating_sub(*minimum);
        if !self.received.is_empty() && self.in_flight <= self.start_in_flight {
            if now - self.start_ns < SETTLE_NS {
                return false;
            }
            let expected = scheduled - self.scheduled;
            if received
                .iter()
                .zip(&self.received)
                .all(|(current, previous)| current - previous + RECEIVE_QUANTUM >= expected)
            {
                return true;
            }
        }
        self.received.clear();
        self.received.extend_from_slice(received);
        self.scheduled = scheduled;
        self.start_ns = now;
        self.start_in_flight = self.in_flight;
        false
    }
}

pub fn read_phase_count(command: &str, size: usize) -> u64 {
    let prefix = format!("{command},{size},");
    for line in io::stdin().lock().lines() {
        let line = line.expect("phase control read");
        if let Some(count) = line.strip_prefix(&prefix) {
            return count.parse().expect("phase count");
        }
        if matches!(line.trim(), "STOP" | "QUIT") {
            break;
        }
    }
    panic!("missing phase control {command}");
}
