import os
import queue
import sys
import threading
import time

from perf_metrics import HEADER_MAGIC, HEADER_SIZE, decode_header


MAX_BURST_PER_SENDER = 64
SETTLE_SECONDS = 0.5
RECEIVE_QUANTUM = 1
PHASE_WARMUP = 0
PHASE_ACTIVE = 1
PHASE_LATENCY = 3
NS_PER_SECOND = 1_000_000_000


def wait_ms(now, until):
    remaining_ns = round(until * NS_PER_SECOND) - round(now * NS_PER_SECOND)
    return min(50, max(0, remaining_ns // 1_000_000))


def active_receive(observed_ns, deadline_ns):
    return observed_ns < deadline_ns


def latency_duration(active_seconds):
    try:
        seconds = int(os.environ.get("PERF_MULTI_LATENCY_DURATION_SECONDS", "0"))
    except ValueError:
        seconds = 0
    return seconds if seconds > 0 else active_seconds


class Schedule:
    def __init__(self, epoch, received, duration):
        self.epoch = epoch
        self.rate = received / duration * 0.8
        self.reserved = 0
        self.accepted = 0
        self.late = 0

    def next(self):
        return self.epoch + self.reserved / self.rate

    def due(self, now):
        return self.rate > 0 and now >= self.next()

    def reserve(self):
        due = self.next()
        self.reserved += 1
        return due

    def admit(self, due, now):
        self.accepted += 1
        if now - due > 0.001:
            self.late += 1

    def phase(self, now):
        return PHASE_WARMUP if now < self.epoch + SETTLE_SECONDS else PHASE_LATENCY

    def scheduled(self, elapsed):
        return int(self.rate * max(0, elapsed))

    def print(self, pattern, args, active_sends, received, paced_received, caught_up=None):
        print(
            f"BENCH_DIAG,{pattern},{args.msg_size},active_sender_rate,{active_sends / args.duration:.12g}"
            f",target_sender_rate,{self.rate:.12g},latency_sends,{self.accepted}"
            f",late_sends_gt_1ms,{self.late},received_rate,{received / args.duration:.12g}"
            f",paced_received_rate,{paced_received / latency_duration(args.duration):.12g}"
            f",caught_up,{'yes' if caught_up is not None else 'no'}"
            f",catch_up_seconds,{caught_up if caught_up is not None else -1:.12g}",
            flush=True,
        )


class CatchUp:
    def __init__(self):
        self.received = ()
        self.scheduled = 0
        self.start = 0
        self.start_in_flight = 0
        self.in_flight = 0

    def observe(self, elapsed, received, scheduled):
        if not received:
            return False
        self.in_flight = max(0, scheduled - min(received))
        if self.received and self.in_flight <= self.start_in_flight:
            if elapsed - self.start < SETTLE_SECONDS:
                return False
            expected = scheduled - self.scheduled
            if all(
                count - initial + RECEIVE_QUANTUM >= expected
                for count, initial in zip(received, self.received)
            ):
                return True
        self.received = tuple(received)
        self.scheduled = scheduled
        self.start = elapsed
        self.start_in_flight = self.in_flight
        return False


def metric_header(data, size, run_id, *, marker=False):
    if len(data) != (HEADER_SIZE if marker else max(size, HEADER_SIZE)):
        return None
    header = decode_header(data)
    if (
        header["magic"] != HEADER_MAGIC
        or header["run_id"] != run_id
        or header["msg_size"] != size
    ):
        return None
    return header


def record_receive(data, size, run_id, observed, deadline, sampler, divisor=1, *, sample_start=None):
    header = metric_header(data, size, run_id)
    if header is None:
        return False
    if sampler is None:
        return header["phase"] == PHASE_ACTIVE and active_receive(observed, deadline)
    sent = header["sent_ts_ns"]
    if header["phase"] == PHASE_LATENCY and 0 < sent <= observed:
        sampler.add((observed - sent) / divisor)
    return (
        header["phase"] in (PHASE_WARMUP, PHASE_LATENCY)
        and sample_start <= observed < deadline
    )


def read_phase_count(command, size):
    prefix = f"{command},{size},"
    for line in sys.stdin:
        if line.startswith(prefix):
            return int(line.removeprefix(prefix))
        if line.strip() in {"STOP", "QUIT"}:
            break
    raise RuntimeError(f"missing phase control {command}")


def phase_commands():
    commands = queue.SimpleQueue()

    def read():
        for line in sys.stdin:
            text = line.strip()
            commands.put((text, time.monotonic()))
            if text in {"STOP", "QUIT"}:
                return

    threading.Thread(target=read, daemon=True).start()
    return commands
