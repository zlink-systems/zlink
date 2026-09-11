package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkBoundSessionReplyCapabilityRuntimePortTest {
    @Test
    void acceptedRequestRepliesThroughItsOriginalPhysicalSessionAfterRebind()
        throws Exception {
        RoutingId actorNodeRid = RoutingId.from("reply-capability-actor");
        RoutingId sessionNodeRid = RoutingId.from("reply-capability-session");
        RoutingId sessionRid = RoutingId.from("reply-capability-client");
        String endpoint = "inproc://jvm-reply-capability-" + System.nanoTime();
        String sessionEndpoint =
            "inproc://jvm-reply-capability-session-" + System.nanoTime();
        CompletableFuture<List<ZLinkBackendActorReceived>> accepted =
            new CompletableFuture<>();
        CompletableFuture<String> originalReply = new CompletableFuture<>();
        CopyOnWriteArrayList<String> replacementReplies =
            new CopyOnWriteArrayList<>();

        try (var context = Zlink.createContext();
             var actorNode = new ZLinkJavaRawMeshNode(context, "mesh");
             var sessionNode = new ZLinkJavaRawMeshNode(context, "mesh");
             var originalSession = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(),
                 sessionNode,
                 (rid, parts, flags) -> {
                     assertEquals(sessionRid, rid);
                     originalReply.complete(frameBody(parts));
                     return true;
                 });
             var replacementSession = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(),
                 sessionNode,
                 (rid, parts, flags) -> {
                     assertEquals(sessionRid, rid);
                     replacementReplies.add(frameBody(parts));
                     return true;
                 })) {
            actorNode.setRoutingId(actorNodeRid);
            actorNode.setBind(endpoint);
            sessionNode.setRoutingId(sessionNodeRid);
            sessionNode.setBind(sessionEndpoint);
            actorNode.start();
            sessionNode.start();
            actorNode.setPeerAuthorityResolver(
                (meshName, candidateRid, candidateGeneration) ->
                    CompletableFuture.completedFuture(Optional.of(
                        new ZLinkInternalMeshNode.PeerAuthorityFence(
                            candidateRid,
                            candidateGeneration,
                            "session-owner",
                            1))));
            actorNode.refreshLocalAuthorityFence().toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            actorNode.observePeerAdmissionExpectation(
                sessionNodeRid,
                sessionEndpoint,
                sessionNode.lifecycleGeneration(),
                systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                "session-owner",
                1);
            sessionNode.connectPeer(endpoint, actorNodeRid);
            awaitAdmitted(sessionNode);

            ZLinkBackendSpot entry = actorNode.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    accepted.complete(List.copyOf(info.actorMessages()));
                }
            });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = actorNode.spotNode().createActor(
                    "reply-capability-actor", create);
            }
            actorNode.spotNode().rememberActorAuthority(actor, 73, 1);
            sessionNode.spotNode().rememberActorAuthority(actor, 73, 1);

            originalSession.startSessionService();
            replacementSession.startSessionService();
            originalSession.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            long originalBinding = originalSession.boundActorBindingGeneration(
                sessionRid, actor.actorId());
            ZLinkStreamHeader requestHeader = new ZLinkStreamHeader(
                "BoundRequest", Map.of(), Optional.of(91L));
            try (Message encodedHeader = Message.from(
                     ZLinkStreamHeaderCodec.encode(requestHeader));
                 Message payload = Message.from("request")) {
                assertTrue(((ZLinkJavaRawSpotNode) sessionNode.spotNode())
                    .forwardBoundStreamSession(
                        actor,
                        sessionRid,
                        originalBinding,
                        1,
                        originalSession,
                        requestHeader,
                        List.of(encodedHeader, payload)));
            }

            List<ZLinkBackendActorReceived> frames =
                accepted.get(1, TimeUnit.SECONDS);
            replacementSession.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            assertTrue(
                replacementSession.boundActorBindingGeneration(
                    sessionRid, actor.actorId()) > originalBinding);

            ZLinkBackendActorReceived capability = frames.getFirst();
            byte[] replyFrame = ZLinkStreamFrameCodec.encode(
                ZLinkStreamMessageKind.RESPONSE,
                ZLinkStreamCodec.RAW,
                Optional.of(91L),
                "",
                "original-reply".getBytes(StandardCharsets.UTF_8));
            try (Message reply = Message.from(replyFrame)) {
                actorNode.spotNode().replyActorNoBind(
                    capability.actor(),
                    capability.sourceNodeRid(),
                    capability.sourceSessionRid(),
                    capability.requestId(),
                    capability.flags(),
                    List.of(reply));
            } finally {
                frames.forEach(ZLinkBackendActorReceived::close);
            }

            assertEquals(
                "original-reply", originalReply.get(1, TimeUnit.SECONDS));
            Thread.sleep(25);
            assertFalse(replacementReplies.contains("original-reply"));
        }
    }

    @ParameterizedTest
    @EnumSource(AuthorityLookupFailure.class)
    void establishedBindingDispatchesWithoutRepeatingAuthorityStoreLookup(
        AuthorityLookupFailure lookupFailure) throws Exception {
        RoutingId actorNodeRid = RoutingId.from("authority-actor");
        RoutingId sessionNodeRid = RoutingId.from("authority-session");
        RoutingId sessionRid = RoutingId.from("authority-client");
        String endpoint = "inproc://jvm-authority-actor-" + System.nanoTime();
        AtomicReference<AuthorityLookupFailure> activeFailure =
            new AtomicReference<>();
        AtomicInteger failedLookups = new AtomicInteger();
        CopyOnWriteArrayList<String> packets = new CopyOnWriteArrayList<>();
        CompletableFuture<Void> probeHandled = new CompletableFuture<>();

        try (var context = Zlink.createContext();
             var actorNode = new ZLinkJavaRawMeshNode(context, "mesh");
             var sessionNode = new ZLinkJavaRawMeshNode(context, "mesh");
             var session = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(),
                 sessionNode,
                 (rid, parts, flags) -> true)) {
            actorNode.setRoutingId(actorNodeRid);
            actorNode.setBind(endpoint);
            sessionNode.setRoutingId(sessionNodeRid);
            sessionNode.setBind(
                "inproc://jvm-authority-session-" + System.nanoTime());
            actorNode.setPeerAuthorityResolver(
                (meshName, candidateRid, candidateGeneration) -> {
                    AuthorityLookupFailure active = activeFailure.get();
                    if (active != null && active.matches(
                            candidateRid, actorNodeRid, sessionNodeRid)) {
                        failedLookups.incrementAndGet();
                        if (active == AuthorityLookupFailure.LOCAL_FAILURE) {
                            return CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "controlled authority lookup failure"));
                        }
                        return CompletableFuture.completedFuture(
                            Optional.empty());
                    }
                    return CompletableFuture.completedFuture(Optional.of(
                        new ZLinkInternalMeshNode.PeerAuthorityFence(
                            candidateRid,
                            candidateGeneration,
                            "owner-" + candidateRid,
                            1)));
                });
            actorNode.start();
            sessionNode.start();
            actorNode.refreshLocalAuthorityFence().toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            actorNode.observePeerAdmissionExpectation(
                sessionNodeRid,
                sessionNode.status().localEndpoint(),
                sessionNode.lifecycleGeneration(),
                systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                "owner-" + sessionNodeRid,
                1);
            sessionNode.connectPeer(endpoint, actorNodeRid);
            awaitAdmitted(sessionNode);

            ZLinkBackendSpot entry = actorNode.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event() != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                try {
                    for (int index = 0; index < info.actorMessages().size();) {
                        ZLinkBackendActorReceived first =
                            info.actorMessages().get(index);
                        String packetName = ZLinkStreamHeaderCodec
                            .decodeOrPlain(first.message().data())
                            .packetName();
                        packets.add(packetName);
                        if (packetName.equals("AuthorityProbe")) {
                            probeHandled.complete(null);
                        }
                        do {
                            index++;
                        } while (index < info.actorMessages().size()
                            && info.actorMessages().get(index - 1).hasMore());
                    }
                } finally {
                    info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                }
            });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = actorNode.spotNode().createActor(
                    "authority-actor", create);
            }
            actorNode.spotNode().rememberActorAuthority(actor, 73, 1);
            sessionNode.spotNode().rememberActorAuthority(actor, 73, 1);

            session.startSessionService();
            activeFailure.set(lookupFailure);
            session.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            long bindingGeneration = session.boundActorBindingGeneration(
                sessionRid, actor.actorId());

            assertTrue(forwardBoundActor(
                sessionNode, session, actor, sessionRid, bindingGeneration,
                24, "AuthorityProbe"));
            probeHandled.get(1, TimeUnit.SECONDS);

            assertEquals(0, failedLookups.get(),
                "commands 38 and 24 must not repeat authority Store lookup");
            assertEquals(1, packets.stream()
                .filter("AuthorityProbe"::equals).count(),
                "command 24 must use the established binding authority");
        }
    }

    private static boolean forwardBoundActor(
        ZLinkJavaRawMeshNode sessionNode,
        ZLinkJavaStreamSocket session,
        ZLinkBackendActorRef actor,
        RoutingId sessionRid,
        long bindingGeneration,
        long sequence,
        String packetName) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            packetName, Map.of(), Optional.empty());
        try (Message encodedHeader = Message.from(
                 ZLinkStreamHeaderCodec.encode(header));
             Message payload = Message.from("payload")) {
            return ((ZLinkJavaRawSpotNode) sessionNode.spotNode())
                .forwardBoundStreamSession(
                    actor,
                    sessionRid,
                    bindingGeneration,
                    sequence,
                    session,
                    header,
                    List.of(encodedHeader, payload));
        }
    }

    private static String frameBody(List<Message> parts) {
        byte[] body = ZLinkStreamFrameCodec.tryDecode(
                parts.getLast().toByteArray())
            .orElseThrow()
            .body();
        return new String(body, StandardCharsets.UTF_8);
    }

    private static void awaitAdmitted(ZLinkJavaRawMeshNode node)
        throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream().noneMatch(peer ->
                peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED)
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(node.peers().stream().anyMatch(peer ->
            peer.state()
                == systems.zlink.framework.runtime.internal.binding.spot
                    .MeshPeerState.ADMITTED));
    }

    private enum AuthorityLookupFailure {
        LOCAL_EMPTY,
        LOCAL_FAILURE,
        SOURCE_EMPTY;

        private boolean matches(
            RoutingId candidate,
            RoutingId local,
            RoutingId source) {
            return this == SOURCE_EMPTY
                ? candidate.equals(source)
                : candidate.equals(local);
        }
    }
}
