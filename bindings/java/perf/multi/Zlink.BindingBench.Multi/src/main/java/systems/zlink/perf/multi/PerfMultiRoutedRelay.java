/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;

/** Common asynchronous routed echo relay used by SENDSEND multi benchmarks. */
final class PerfMultiRoutedRelay {
    private static final int ENOTCONN_WIN = 10057;
    private static final int EHOSTUNREACH_WIN = 10065;

    private PerfMultiRoutedRelay() {
    }

    static void run(RouterSocket server, AtomicBoolean stopRequested) {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        try (Received received = new Received();
             PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                 List.of(server), PollEventFlags.POLLIN)) {
            while (!stopRequested.get() && failure.get() == null) {
                int readyCount = pollSet.poll(50);
                if (readyCount <= 0
                    || !pollSet.readyHasEventAt(0, PollEventFlags.POLLIN)) {
                    continue;
                }
                drainRequests(server, received, stopRequested, failure);
            }
        }

        Throwable error = failure.get();
        if (error != null) {
            throw new IllegalStateException("multi routed relay failed", error);
        }
    }

    private static void drainRequests(RouterSocket server,
                                      Received received,
                                      AtomicBoolean stopRequested,
                                      AtomicReference<Throwable> failure) {
        while (!stopRequested.get() && failure.get() == null) {
            boolean ok;
            try {
                ok = server.recv(received, RecvFlags.DONT_WAIT);
            } catch (ZlinkRecvException error) {
                if (error.getResult() == RecvResult.NO_DATA
                    || error.getResult() == RecvResult.BUSY) {
                    return;
                }
                throw error;
            }
            if (!ok) {
                return;
            }

            RoutingId routingId = RoutingId.from(
                received.getRoutingId().orElseThrow().toBytes());
            Message payload = PerfUtil.measurementPayload(received.parts());
            if (payload != null) {
                submitReply(server, routingId, payload, stopRequested,
                    failure);
            }
            received.close();
        }
    }

    private static void submitReply(RouterSocket server,
                                    RoutingId routingId,
                                    Message source,
                                    AtomicBoolean stopRequested,
                                    AtomicReference<Throwable> failure) {
        CompletionStage<Void> stage;
        try (Message payload = Message.from(source);
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (tail != null) {
                stage = server.send(routingId).message(payload).message(tail)
                    .submit();
            } else {
                stage = server.send(routingId).message(payload).submit();
            }
        } catch (Throwable error) {
            recordFailure(error, stopRequested, failure);
            return;
        }

        stage.whenComplete((ignored, error) -> {
            if (error != null) {
                recordFailure(error, stopRequested, failure);
            }
        });
    }

    private static void recordFailure(Throwable error,
                                      AtomicBoolean stopRequested,
                                      AtomicReference<Throwable> failure) {
        Throwable cause = PerfMultiAsyncSendLoop.completionCause(error);
        if (isStaleRoute(cause) || stopRequested.get()) {
            return;
        }
        failure.compareAndSet(null, cause);
        stopRequested.set(true);
    }

    static boolean isStaleRoute(Throwable error) {
        if (!(error instanceof ZlinkException zlink)) {
            return false;
        }
        if (zlink instanceof ZlinkSubmitException submit
            && (submit.getResult() == SubmitResult.NOT_CONNECTED
                || submit.getResult() == SubmitResult.NOT_FOUND)) {
            return true;
        }
        int errno = zlink.getNativeErrno();
        return errno == PerfErrno.ENOTCONN
            || errno == PerfErrno.EHOSTUNREACH
            || errno == ENOTCONN_WIN
            || errno == EHOSTUNREACH_WIN;
    }
}
