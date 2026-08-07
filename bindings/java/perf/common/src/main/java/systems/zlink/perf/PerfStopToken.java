/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Wire-level stop token shared by single + multi perf benchmarks.
 *
 * <p>Senders emit the literal byte sequence {@code __zlink_perf_stop__} on the
 * wire once at the end of an active phase to signal phase completion to a
 * receiver waiting on a poller. Receivers exit when {@link #isStopToken(byte[])}
 * (or {@link #isStopTokenMessage(Message)}) returns {@code true}.
 *
 * <p>This mirrors the C++ {@code k_stop_token} / {@code is_stop_token} helpers
 * in {@code bindings/cpp/perf/single/common/perf_single_common.hpp} and the
 * multi-side {@code is_stop_token} helper, and follows the policy in
 * {@code doc/perf/PERF_SINGLE_TEST_POLICY.md} § 1.4 and
 * {@code doc/perf/PERF_MULTI_TEST_POLICY.md} § 1.3.1.
 *
 * <p>The token replaces {@code AtomicBoolean senderDone} + short polling
 * patterns. Receivers must use {@code Duration.ofMillis(-1)} (signal-driven
 * indefinite wait) and exit purely on stop-token arrival.
 */
public final class PerfStopToken {
    /** Literal stop-token byte sequence, shared across all bindings. */
    public static final byte[] BYTES =
        "__zlink_perf_stop__".getBytes(StandardCharsets.UTF_8);

    private PerfStopToken() {
    }

    /** Returns {@code true} if the supplied bytes are exactly the stop token. */
    public static boolean isStopToken(byte[] data) {
        return data != null && Arrays.equals(data, BYTES);
    }

    /** Returns {@code true} if the message frame contents are the stop token. */
    public static boolean isStopTokenMessage(Message message) {
        if (message == null) {
            return false;
        }
        if (message.size() != BYTES.length) {
            return false;
        }
        return Arrays.equals(message.data(), BYTES);
    }

    /** Allocates a new {@link Message} carrying the stop-token bytes. */
    public static Message newMessage() {
        return Message.from(BYTES);
    }

    /**
     * Bounded retry of a non-blocking stop-token send through transient
     * backpressure, mirroring the C reference
     * {@code perf_single_one_way.hpp::send_stop_token_with_retry}
     * (~188-205): a 10-second default deadline with a 1ms sleep between
     * transient failures so the receiver always observes the wire-level
     * terminator. The deadline is configurable for diagnostics with
     * {@code PERF_STOP_RETRY_TIMEOUT_MS}.
     *
     * <p>A single blocking submit is NOT equivalent: under load a ROUTER
     * peer drops on a missing route (EHOSTUNREACH) and a send-timeout-bound
     * socket can fail on EAGAIN/ETIMEDOUT, losing the only stop token and
     * hanging a receiver parked on {@code poll(-1)}.
     *
     * @param attempt a single non-blocking ({@code DONT_WAIT}) stop-token
     *                send attempt that returns {@code true} once the token is
     *                accepted and {@code false} on transient backpressure.
     * @param label   diagnostic label used in failure messages.
     */
    public static void sendWithRetry(java.util.function.BooleanSupplier attempt,
                                     String label) {
        sendWithRetry(attempt, () -> {
            try {
                Thread.sleep(1L);
            } catch (InterruptedException ex) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException(
                    label + " stop-token send interrupted", ex);
            }
        }, label);
    }

    /**
     * Retries a non-blocking stop-token send after a caller-supplied readiness
     * wait. Socket callers should use a POLLOUT poller here so the retry does
     * not turn backpressure into a timer-based hot loop.
     */
    public static void sendWithRetry(java.util.function.BooleanSupplier attempt,
                                     Runnable waitForRetry,
                                     String label) {
        int timeoutMs = Math.max(1,
            PerfUtil.intEnv("PERF_STOP_RETRY_TIMEOUT_MS", 10_000));
        long deadline = System.nanoTime() + timeoutMs * 1_000_000L;
        while (System.nanoTime() < deadline) {
            if (attempt.getAsBoolean()) {
                return;
            }
            waitForRetry.run();
        }
        throw new IllegalStateException(
            label + " stop-token send exceeded " + timeoutMs + "ms retry deadline");
    }
}
