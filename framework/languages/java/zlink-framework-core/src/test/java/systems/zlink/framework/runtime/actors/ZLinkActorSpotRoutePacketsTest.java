package systems.zlink.framework.runtime.actors;
import java.util.Map;
import java.util.Optional;

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
                    "ProbeReq", Map.of(), Optional.of(7L)),
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
                    "DeferredSend", Map.of(), Optional.empty()),
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

    /**
     * Spec 15 §4.2: the Join Accepted admission reply carries the target's
     * advertised relocation state chunk receive limit alongside
     * {@code accepted}.
     */
    @Test
    void admissionReplyRoundTripsAcceptedAndAdvertisedChunkLimit() {
        try (Message reply = Message.from("payload")) {
            List<Message> encoded = ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                true, "spot-1", 3L, 5L, 32_768L, reply);
            try (ZLinkActorSpotRoutePackets.AdmissionReply decoded =
                ZLinkActorSpotRoutePackets.decodeAdmissionReply(encoded)) {
                assertTrue(decoded.accepted());
                assertEquals("spot-1", decoded.spotId());
                assertEquals(3L, decoded.spotGeneration());
                assertEquals(5L, decoded.membershipEpoch());
                assertEquals(32_768L, decoded.receiveChunkLimitBytes());
                assertArrayEquals(
                    "payload".getBytes(java.nio.charset.StandardCharsets.UTF_8),
                    decoded.reply().toByteArray());
            } finally {
                encoded.forEach(Message::close);
            }
        }
    }

    @Test
    void admissionReplyRoundTripsRejected() {
        try (Message reply = Message.from(new byte[0])) {
            List<Message> encoded = ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                false, null, 0L, 0L, 0L, reply);
            try (ZLinkActorSpotRoutePackets.AdmissionReply decoded =
                ZLinkActorSpotRoutePackets.decodeAdmissionReply(encoded)) {
                assertTrue(!decoded.accepted());
                assertEquals(0, decoded.reply().toByteArray().length);
            } finally {
                encoded.forEach(Message::close);
            }
        }
    }
}
