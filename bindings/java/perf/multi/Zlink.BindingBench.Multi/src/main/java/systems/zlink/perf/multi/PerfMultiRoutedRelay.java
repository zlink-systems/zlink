/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendSubmission;
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
        // Reply admission is asynchronous. A blocking submit is a `NONE FINAL`
        // send that only waits out its SNDTIMEO snapshot and then reports
        // BACKPRESSURED/EAGAIN (core socket spec, part send and pending
        // admission), which turns ordinary echo backpressure into a relay
        // failure. The public async submit lets Core pace admission and owns
        // the WRITABLE retry, exactly like the C relay's retry snapshot.
        PerfMultiRoutedReplyQueue<PendingReply> replies =
            new PerfMultiRoutedReplyQueue<>(
                reply -> submitReply(
                    server, reply.routingId(), reply.parts()),
                PendingReply::close,
                cause -> isStaleRoute(cause) || stopRequested.get());
        try (Received received = new Received();
             PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                 List.of(server), PollEventFlags.POLLIN)) {
            while (!stopRequested.get() && !replies.hasFailure()) {
                int readyCount = pollSet.poll(50);
                if (readyCount <= 0
                    || !pollSet.readyHasEventAt(0, PollEventFlags.POLLIN)) {
                    continue;
                }
                drainRequests(server, received, stopRequested, replies);
            }
            if (!replies.drain(
                    PerfMultiRoutedSendCoordinator.sendDrainTimeout())
                && !replies.hasFailure()) {
                System.err.println("RELAY_DRAIN_DETAIL,timed_out,pending="
                    + replies.pendingCount() + ",sending=" + replies.sending());
            }
        }

        Throwable error = replies.failure();
        if (error != null) {
            String detail = describe(error);
            // The FAIL line keeps only the deepest non-blank message, and a
            // zlink exception carries none. Name the result and errno here and
            // in the server log so the reason is never lost.
            System.err.println("RELAY_FAILURE_DETAIL," + detail);
            throw new IllegalStateException(
                "multi routed relay failed:" + detail, error);
        }
    }

    private static void drainRequests(
            RouterSocket server,
            Received received,
            AtomicBoolean stopRequested,
            PerfMultiRoutedReplyQueue<PendingReply> replies) {
        while (!stopRequested.get() && !replies.hasFailure()) {
            // Do not pull another request out of Core's receive queue while
            // the previous reply is still awaiting admission. The C relay
            // refuses a second reply under a live wait token; parking here
            // keeps the un-forwarded reply backpressuring its source through
            // Core's receive queue instead of an unbounded application queue.
            if (!replies.awaitIdle(50L)) {
                continue;
            }
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
                // Public submit consumes the received parts before returning
                // its admission stage, so closing and refilling the envelope
                // cannot reclaim a part retained by the pending send.
                replies.enqueue(new PendingReply(routingId, received.parts()));
            }
            received.close();
        }
    }

    static SendSubmission submitReply(RouterSocket server,
                                      RoutingId routingId,
                                      List<Message> parts) {
        if (parts.size() == 2) {
            return server.send(routingId)
                .message(parts.get(0))
                .message(parts.get(1))
                .submit();
        }
        return server.send(routingId)
            .message(parts.get(0))
            .submit();
    }

    /** Renders a submit failure so the FAIL reason names result and errno. */
    static String describe(Throwable error) {
        if (error == null) {
            return "unknown";
        }
        StringBuilder text = new StringBuilder(
            error.getClass().getSimpleName());
        if (error instanceof ZlinkSubmitException submit) {
            text.append(":result=").append(submit.getResult());
        }
        if (error instanceof ZlinkException zlink) {
            text.append(":errno=").append(zlink.getNativeErrno());
        }
        String message = error.getMessage();
        if (message != null && !message.isBlank()) {
            text.append(':').append(message);
        }
        return text.toString();
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

    /** One routed reply waiting for its public submit turn. */
    private record PendingReply(RoutingId routingId, List<Message> parts) {
        void close() {
            for (Message part : parts) {
                part.close();
            }
        }
    }
}
