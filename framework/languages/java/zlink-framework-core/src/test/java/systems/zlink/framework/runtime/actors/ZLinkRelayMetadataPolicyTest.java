package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkRelayMetadataPolicyTest {
    @Test
    void filtersEachRelayDirectionWithAnImmutableSnapshot() {
        ZLinkRelayMetadataPolicy policy =
            new ZLinkRelayMetadataPolicy(Set.of("session"), Set.of("actor"));
        ZLinkStreamHeader inbound = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "ActorReq",
            Map.of("session", "s", "actor", "a", "private", "p"));

        ZLinkStreamHeader sessionToActor = policy.sessionToActor(inbound);
        ZLinkBoundSessionSendOptions actorToSession = policy.actorToSession(
            ZLinkBoundSessionSendOptions.create("ActorEvent", ZLinkStreamCodec.JSON)
                .withMetadata("session", "s")
                .withMetadata("actor", "a")
                .withMetadata("private", "p"));

        assertEquals(Map.of("session", "s"), sessionToActor.metadata());
        assertEquals(Map.of("actor", "a"), actorToSession.metadata());
        assertThrows(
            UnsupportedOperationException.class,
            () -> sessionToActor.metadata().put("late", "value"));
        assertThrows(
            UnsupportedOperationException.class,
            () -> actorToSession.metadata().put("late", "value"));
    }

    @Test
    void emptyPolicyDropsApplicationMetadataInBothDirections() {
        ZLinkStreamHeader inbound = new ZLinkStreamHeader(
            "ActorSend",
            Map.of("private", "p"),
            Optional.empty());

        assertEquals(Map.of(), ZLinkRelayMetadataPolicy.EMPTY.sessionToActor(inbound).metadata());
        assertEquals(
            Map.of(),
            ZLinkRelayMetadataPolicy.EMPTY.actorToSession(
                ZLinkBoundSessionSendOptions.create("ActorEvent", ZLinkStreamCodec.JSON)
                    .withMetadata("private", "p"))
                .metadata());
    }
}
