// SPDX-License-Identifier: MPL-2.0

/** Invoked on each timer expiration with the timer and the cumulative fire count; runs on a background dispatch thread. */
export type TimerHandler = (timer: Timer, fireCount: bigint) => void;

/** A timer that fires on an interval and can be polled or awaited. */
export interface Timer {
  /** Start the timer firing once per `intervalNs` nanoseconds; `repeatCount` sets how many times it fires. */
  start(intervalNs: bigint, repeatCount: bigint): void;
  /** Stop the timer; it can be restarted with {@link Timer.start}. */
  stop(): void;
  /** Receive the next expiration as the cumulative fire count, or null when none is pending. */
  recv(): bigint | null;
  /** Register a callback invoked on each expiration; it runs on a background dispatch thread. */
  onFire(handler: TimerHandler): void;
  /** Close the timer and release its resources. */
  close(): void;
}

/** A handle to a running background thread. */
export interface Thread {
  /** Block the caller until the thread finishes running its task. */
  join(): void;
}

/** A high-resolution elapsed-time stopwatch. */
export interface Stopwatch {
  /** Return the elapsed time so far, in microseconds, without stopping. */
  intermediate(): number;
  /** Stop the stopwatch and return the total elapsed time in microseconds. */
  stop(): number;
  /** Release the stopwatch. */
  close(): void;
}

/** A thread-safe integer counter that can be shared across threads. */
export interface AtomicCounter {
  /** Atomically set the value. */
  set(value: number): void;
  /** Atomically increment the counter and return the new value. */
  inc(): number;
  /** Atomically decrement the counter and return the new value. */
  dec(): number;
  /** Return the current value, read atomically. */
  value(): number;
  /** Release the counter. */
  close(): void;
}
