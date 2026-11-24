package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.util.List;
import java.util.UUID;

final class ZLinkSpotRetireSessionRouteOrderTest {
    private static final String SPOT_ID = "forest-2";
    private static final String FIRST_ACTOR_ID = "z";
    private static final String SECOND_ACTOR_ID = "aa";

    @RepeatedTest(40)
    void stageRequestAcceptsRoutesInParticipantOrderWhenActorIdOrderDiffers() {
        var request = request(List.of(route(FIRST_ACTOR_ID), route(SECOND_ACTOR_ID)));

        assertEquals(
                List.of(FIRST_ACTOR_ID, SECOND_ACTOR_ID),
                request.sessionRoutes().stream()
                        .map(ZLinkSpotRetireControl.SessionRouteFence::actorId)
                        .toList());
        assertEquals(
                List.of(FIRST_ACTOR_ID, SECOND_ACTOR_ID, SPOT_ID),
                request.participants().stream()
                        .map(ZLinkSpotRetireControl.ParticipantFence::objectId)
                        .toList());
    }

    @Test
    void stageRequestRejectsDuplicateActorRoutes() {
        assertThrows(
                IllegalArgumentException.class,
                () -> request(List.of(route(FIRST_ACTOR_ID), route(FIRST_ACTOR_ID))));
    }

    @Test
    void stageRequestRejectsSpotRoutesAndMismatchedActorFences() {
        var spotRoute =
                new ZLinkSpotRetireControl.SessionRouteFence(
                        SPOT_ID,
                        13,
                        19,
                        "store-spot",
                        RoutingId.from("session-owner-spot"),
                        31,
                        "session-owner-spot",
                        41,
                        RoutingId.from("session-spot"),
                        51);
        assertThrows(IllegalArgumentException.class, () -> request(List.of(spotRoute)));

        var wrongObjectGeneration =
                new ZLinkSpotRetireControl.SessionRouteFence(
                        FIRST_ACTOR_ID,
                        999,
                        17,
                        "store-z",
                        RoutingId.from("session-owner-z"),
                        31,
                        "session-owner-z",
                        41,
                        RoutingId.from("session-z"),
                        51);
        assertThrows(IllegalArgumentException.class, () -> request(List.of(wrongObjectGeneration)));

        var wrongAuthorityGeneration =
                new ZLinkSpotRetireControl.SessionRouteFence(
                        FIRST_ACTOR_ID,
                        11,
                        999,
                        "store-z",
                        RoutingId.from("session-owner-z"),
                        31,
                        "session-owner-z",
                        41,
                        RoutingId.from("session-z"),
                        51);
        assertThrows(
                IllegalArgumentException.class, () -> request(List.of(wrongAuthorityGeneration)));
    }

    private static ZLinkSpotRetireControl.StageRequest request(
            List<ZLinkSpotRetireControl.SessionRouteFence> sessionRoutes) {
        return new ZLinkSpotRetireControl.StageRequest(
                new ZLinkSpotRetireControl.Fence(UUID.randomUUID(), 1),
                RoutingId.from("source-node"),
                11,
                "source-owner",
                12,
                RoutingId.from("target-node"),
                13,
                "target-owner",
                14,
                "mesh",
                SPOT_ID,
                "room",
                false,
                true,
                new byte[] {1},
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                ZLinkAuthorityKeyCodec.actor(FIRST_ACTOR_ID),
                                1,
                                FIRST_ACTOR_ID,
                                "player",
                                true,
                                11,
                                17),
                        new ZLinkSpotRetireControl.ParticipantFence(
                                ZLinkAuthorityKeyCodec.actor(SECOND_ACTOR_ID),
                                1,
                                SECOND_ACTOR_ID,
                                "player",
                                true,
                                12,
                                18),
                        new ZLinkSpotRetireControl.ParticipantFence(
                                ZLinkAuthorityKeyCodec.spot(SPOT_ID),
                                2,
                                SPOT_ID,
                                "room",
                                true,
                                13,
                                19)),
                sessionRoutes);
    }

    private static ZLinkSpotRetireControl.SessionRouteFence route(String actorId) {
        return switch (actorId) {
            case FIRST_ACTOR_ID ->
                    new ZLinkSpotRetireControl.SessionRouteFence(
                            actorId,
                            11,
                            17,
                            "store-z",
                            RoutingId.from("session-owner-z"),
                            31,
                            "session-owner-z",
                            41,
                            RoutingId.from("session-z"),
                            51);
            case SECOND_ACTOR_ID ->
                    new ZLinkSpotRetireControl.SessionRouteFence(
                            actorId,
                            12,
                            18,
                            "store-aa",
                            RoutingId.from("session-owner-aa"),
                            32,
                            "session-owner-aa",
                            42,
                            RoutingId.from("session-aa"),
                            52);
            default -> throw new IllegalArgumentException("unknown ActorId: " + actorId);
        };
    }
}
