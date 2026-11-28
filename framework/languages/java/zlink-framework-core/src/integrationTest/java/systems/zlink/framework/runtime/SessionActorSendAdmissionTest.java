package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.StreamSessionTest.CLIENT_TIMEOUT_MILLIS;
import static systems.zlink.framework.runtime.StreamSessionTest.assertBoundControl;
import static systems.zlink.framework.runtime.StreamSessionTest.assertReply;
import static systems.zlink.framework.runtime.StreamSessionTest.bytes;
import static systems.zlink.framework.runtime.StreamSessionTest.frame;
import static systems.zlink.framework.runtime.StreamSessionTest.requestHeader;
import static systems.zlink.framework.runtime.StreamSessionTest.startActorRelayRuntime;
import static systems.zlink.framework.runtime.StreamSessionTest.streamPort;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.StreamSessionTest.ActorRelaySession;
import systems.zlink.framework.runtime.StreamSessionTest.PlayerActor;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.net.Socket;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class SessionActorSendAdmissionTest {
    private static final String PACKET = "SessionActorSendAdmission";
    private static volatile Turns turns;

    @Test
    void sendCompletesAtMailboxAdmissionWhileHandlerIsPending() throws Exception {
        Turns current = new Turns();
        turns = current;
        try (var runtime = startActorRelayRuntime(AdmissionSession.class);
                var client = new Socket("127.0.0.1", streamPort(runtime))) {
            AdmissionSession session = bind(client);
            try {
                send(client, "\"first\"");
                current.entered.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
                session.firstAccepted.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);

                send(client, "\"queued\"");
                session.secondAccepted.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
                assertFalse(current.queuedPayload.isDone());
            } finally {
                current.release.complete(null);
            }
            assertEquals(
                    "queued",
                    current.queuedPayload.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS));
        } finally {
            turns = null;
        }
    }

    @Test
    void admittedSendReportsHandlerExceptionWithoutFailingRelay() throws Exception {
        ZLinkTelemetryTestFence.awaitIdle();
        Turns current = new Turns();
        turns = current;
        var errors = new CopyOnWriteArrayList<List<String>>();
        var logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var observer =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        var fields = List.of(record.getMessage().split(" "));
                        if (fields.contains("event_id=zlink.dispatch_error")
                                && fields.contains("packet=" + PACKET)) {
                            errors.add(fields);
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(observer);
        try (var runtime = startActorRelayRuntime(AdmissionSession.class);
                var client = new Socket("127.0.0.1", streamPort(runtime))) {
            AdmissionSession session = bind(client);
            try {
                send(client, "\"failing\"");
                current.entered.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
                assertDoesNotThrow(
                        () ->
                                session.firstAccepted.get(
                                        CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS),
                        "SEND relay must succeed before the handler barrier opens");
            } finally {
                current.release.complete(null);
            }

            send(client, "\"queued\"");
            assertEquals(
                    "queued",
                    current.queuedPayload.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS));
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(1, errors.size(), errors::toString);
            assertTrue(
                    errors.getFirst()
                            .containsAll(
                                    List.of(
                                            "surface=actor",
                                            "kind=send",
                                            "reason=handler_exception",
                                            "action=drop",
                                            "actor=player-1")),
                    errors::toString);
        } finally {
            logger.removeHandler(observer);
            turns = null;
        }
    }

    @Test
    void sendThroughRemovedBindingCompletesWithRejection() throws Exception {
        turns = new Turns();
        try (var runtime = startActorRelayRuntime(AdmissionSession.class);
                var client = new Socket("127.0.0.1", streamPort(runtime))) {
            AdmissionSession session = bind(client);
            session.bound
                    .notifyDisconnected()
                    .toCompletableFuture()
                    .get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);

            send(client, "\"rejected\"");
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    session.firstAccepted.get(
                                            CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertFalse(turns.queuedPayload.isDone());
        } finally {
            turns = null;
        }
    }

    private static AdmissionSession bind(Socket client) throws Exception {
        client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
        client.getOutputStream().write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
        assertBoundControl(client, "player-1");
        assertReply(client, 1L, "\"bound\"");
        var session = (AdmissionSession) ActorRelaySession.lastConnected;
        session.bound = session.context().actors().bound().getFirst();
        return session;
    }

    private static void send(Socket client, String payload) throws Exception {
        var header =
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.SEND,
                        ZLinkStreamCodec.JSON,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.empty(),
                        PACKET,
                        Map.of());
        client.getOutputStream()
                .write(frame(ZLinkStreamHeaderCodec.encode(header), bytes(payload)));
    }

    public static final class AdmissionSession extends ActorRelaySession {
        private final CompletableFuture<Void> firstAccepted = new CompletableFuture<>();
        private final CompletableFuture<Void> secondAccepted = new CompletableFuture<>();
        private volatile ZLinkSessionActor bound;
        private int sends;

        public AdmissionSession(ZLinkSessionContext context, ZLinkActorManager actors) {
            super(context, actors);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            if (!PACKET.equals(dispatch.packetName())) {
                return super.onDispatch(dispatch, payload);
            }
            var accepted = sends++ == 0 ? firstAccepted : secondAccepted;
            return bound.relay(dispatch, payload)
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure == null) accepted.complete(null);
                                else accepted.completeExceptionally(failure);
                            });
        }
    }

    public static final class AdmissionHandler {
        @ZLinkSpotActorSend(packetName = PACKET)
        public CompletionStage<Void> handle(PlayerActor actor, String payload) {
            Turns current = turns;
            if ("first".equals(payload) || "failing".equals(payload)) {
                current.entered.complete(null);
                return "first".equals(payload)
                        ? current.release
                        : current.release.thenRun(
                                () -> {
                                    throw new IllegalStateException("admitted SEND handler failed");
                                });
            }
            current.queuedPayload.complete(payload);
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class Turns {
        private final CompletableFuture<Void> entered = new CompletableFuture<>();
        private final CompletableFuture<Void> release = new CompletableFuture<>();
        private final CompletableFuture<String> queuedPayload = new CompletableFuture<>();
    }
}
