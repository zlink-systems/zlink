package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkLocalActorFollowAdmissionTest {
    enum Relay {
        NONE,
        SUCCESS_AFTER_ADMISSION,
        FAILURE_BEFORE_ADMISSION,
        FAILURE_AFTER_ADMISSION
    }

    @ParameterizedTest
    @EnumSource(Relay.class)
    void localIngressUsesFollowerAndKeepsAdmissionSeparateFromRelayTerminal(Relay relay) {
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "mesh")) {
            owner.setRoutingId(RoutingId.from("local-follow-admission"));
            owner.setBind("inproc://local-follow-admission-" + System.nanoTime());
            owner.start();
            owner.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    java.util.Optional.of(
                                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "owner", 3))));
            owner.refreshLocalAuthorityFence().toCompletableFuture().join();
            var spots = (ZLinkJavaRawSpotNode) owner.spotNode();
            ZLinkBackendActorRef actor;
            try (var create = Message.from("create")) {
                actor = spots.createActor("actor", create);
            }
            spots.rememberActorAuthority(actor, 11, 3);
            var dispatches = new AtomicInteger();
            spots.entrySpot()
                    .onDispatchEvent(
                            new ZLinkInternalAsyncSpotDispatchHandler() {
                                @Override
                                public CompletionStage<Void> handleAsync(
                                        ZLinkBackendSpotDispatchInfo info) {
                                    dispatches.incrementAndGet();
                                    info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                                    return CompletableFuture.completedFuture(null);
                                }
                            });
            var calls = new AtomicInteger();
            var retained = new AtomicReference<List<Message>>();
            var fail = new AtomicReference<Consumer<Throwable>>();
            var release = new AtomicReference<Runnable>();
            var expected = new IllegalStateException("owned relay failure");
            spots.setMessageFollowRelayHandler(
                    (rid,
                            generation,
                            header,
                            journal,
                            parts,
                            contentType,
                            reply,
                            failure,
                            terminalRelease) -> {
                        calls.incrementAndGet();
                        var accepted = ZLinkServiceFrozenRecordCodec.decodeActor(journal);
                        assertEquals(header.operationHigh(), accepted.operationHigh());
                        assertEquals(header.operationLow(), accepted.operationLow());
                        assertEquals(actor, header.target().actor());
                        if (relay == Relay.NONE) return false;
                        retained.set(parts);
                        fail.set(failure);
                        release.set(terminalRelease);
                        if (relay == Relay.FAILURE_BEFORE_ADMISSION) {
                            failure.accept(expected);
                            terminalRelease.run();
                        }
                        return true;
                    });
            var reports = new AtomicInteger();
            var logger = Logger.getLogger(ZLinkJavaRawSpotNode.class.getName());
            var observer =
                    new Handler() {
                        @Override
                        public void publish(LogRecord record) {
                            if (record.getThrown() == expected) reports.incrementAndGet();
                        }

                        @Override
                        public void flush() {}

                        @Override
                        public void close() {}
                    };
            logger.addHandler(observer);
            try (var header = Message.from("header");
                    var body = Message.from("body")) {
                var admitted =
                        spots.sendToActorAsync(actor, List.of(header, body)).toCompletableFuture();
                assertEquals(1, calls.get());
                if (relay == Relay.FAILURE_BEFORE_ADMISSION) {
                    assertSame(
                            expected,
                            assertThrows(CompletionException.class, admitted::join).getCause());
                } else {
                    assertTrue(
                            admitted.isDone(),
                            "local relay acceptance must not wait for relay terminal");
                    admitted.join();
                    if (relay != Relay.NONE) {
                        assertEquals(
                                "body",
                                new String(
                                        retained.get().get(1).toByteArray(),
                                        java.nio.charset.StandardCharsets.UTF_8));
                        if (relay == Relay.FAILURE_AFTER_ADMISSION) fail.get().accept(expected);
                        release.get().run();
                        admitted.join();
                    }
                }
                assertEquals(relay == Relay.NONE ? 1 : 0, dispatches.get());
                assertEquals(relay == Relay.FAILURE_AFTER_ADMISSION ? 1 : 0, reports.get());
            } finally {
                if (release.get() != null) release.get().run();
                logger.removeHandler(observer);
            }
        }
    }
}
