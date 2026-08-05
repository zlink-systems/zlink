package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

final class ZLinkActorSpotRoutePacketsTest {
    @Test
    void commitTransferRoundTripsBoundSessionRouteAndCoreAuthority() {
        ZLinkBackendActorRef actor =
            new ZLinkBackendActorRef(RoutingId.from("actor-source"), "actor-1", 17);
        try (Message state = Message.from("state")) {
            List<Message> parts = ZLinkActorSpotRoutePackets.createCommitRequestParts(
                "00000000-0000-0000-0000-000000000041",
                Duration.ofSeconds(12),
                actor.actorId(),
                "stateful",
                actor,
                RoutingId.from("entry-source"),
                "entry-spot",
                "entry-channel",
                RoutingId.from("session-source"),
                RoutingId.from("session-41"),
                "stateful",
                state,
                List.of(),
                new ZLinkActorSpotRoutePackets.CoreTransfer(
                    true, 31L, 41L, 7L, 3L, 5L, 1024L),
                new ZLinkDeferredJoinAcceptedRecovery.Manifest(
                    "relocation-root-41",
                    91L,
                    1));
            try {
                ZLinkActorSpotRoutePackets.TransferRequest decoded =
                    ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));

                assertTrue(decoded.commit());
                assertEquals(actor.nodeRid(), decoded.actorNodeRid());
                assertEquals(actor.generation(), decoded.actorGeneration());
                assertEquals(RoutingId.from("session-source"), decoded.sourceNodeRid());
                assertEquals(RoutingId.from("session-41"), decoded.sourceSessionRid());
                assertTrue(decoded.hasSourceSessionRoute());
                assertTrue(decoded.coreTransfer());
                assertEquals(31L, decoded.coreTransferIdHigh());
                assertEquals(41L, decoded.coreTransferIdLow());
                assertEquals(7L, decoded.coreMembershipEpoch());
                assertEquals(3L, decoded.coreFinalSequence());
                assertEquals(5L, decoded.coreReserveMessageCount());
                assertEquals(1024L, decoded.coreReserveByteCount());
                assertEquals(
                    "relocation-root-41",
                    decoded.completionManifest().reference());
                assertEquals(91L, decoded.completionManifest().checksumCrc32c());
                assertEquals(1, decoded.completionManifest().cursor());
            } finally {
                parts.forEach(Message::close);
            }
        }
    }

    @Test
    void actorPacketAllowsEmptyNativeSourceSessionRoutingId() {
        ZLinkBackendActorRef actor =
            new ZLinkBackendActorRef(RoutingId.from("source"), "actor-1", 3);
        ZLinkActorReplyRoute route = new ZLinkActorReplyRoute(
            actor,
            RoutingId.from("source"),
            RoutingId.from(new byte[0]),
            41L,
            0);
        try (Message payload = Message.from("payload")) {
            List<Message> parts = ZLinkActorSpotRoutePackets.createActorPacketParts(
                actor,
                new ZLinkStreamHeader(
                    "ProbeReq", java.util.Map.of(), java.util.Optional.of(7L)),
                payload,
                route);
            try {
                ZLinkActorSpotRoutePackets.ActorPacket decoded =
                    ZLinkActorSpotRoutePackets.decodeActorPacket(parts);
                try {
                    assertEquals(0, decoded.replyRoute().sourceSessionRid().size());
                    assertEquals(41L, decoded.replyRoute().requestId());
                } finally {
                    decoded.close();
                }
            } finally {
                parts.forEach(Message::close);
            }
        }
    }

    @Test
    void handoffActorPacketRoundTripsCanonicalAcceptedJournal() {
        ZLinkBackendActorRef actor =
            new ZLinkBackendActorRef(RoutingId.from("target"), "actor-1", 9);
        byte[] journal = new byte[] {4, 3, 2, 1};
        try (Message payload = Message.from("payload")) {
            List<Message> parts = ZLinkActorSpotRoutePackets.createActorPacketParts(
                actor,
                new ZLinkStreamHeader(
                    "DeferredSend", java.util.Map.of(), java.util.Optional.empty()),
                payload,
                null,
                17L,
                journal);
            try {
                ZLinkActorSpotRoutePackets.ActorPacket decoded =
                    ZLinkActorSpotRoutePackets.decodeActorPacket(parts);
                try {
                    assertEquals(17L, decoded.handoffArrivalIndex());
                    assertArrayEquals(journal, decoded.acceptedJournalRecord());
                } finally {
                    decoded.close();
                }
            } finally {
                parts.forEach(Message::close);
            }
        }
    }
}
