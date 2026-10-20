package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;

import java.nio.file.Path;
import java.util.HexFormat;

/**
 * Session-Actor binding §4: every runtime encodes the stored Session owner identity unchanged in
 * commands 51 and 54, and the receiving Session owner accepts only an exact match with its current
 * values. The same bytes are checked by the .NET, Node.js and C++ suites.
 */
class ZLinkSessionOwnerIdentityContractTest {
    private static final HexFormat HEX = HexFormat.of();

    @Test
    void commandsCarryTheDescriptorIdentityAndTheSessionOwnerComparesItExactly() throws Exception {
        JsonNode fixture =
                new ObjectMapper()
                        .readTree(
                                Path.of(
                                                "../../../runtime/protocol/fixtures/session-owner-identity-v1.json")
                                        .toFile());
        JsonNode owner = fixture.get("sessionOwner");
        JsonNode session = fixture.get("session");
        JsonNode actor = fixture.get("actor");
        var local =
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                        RoutingId.from(HEX.parseHex(owner.get("nodeRidHex").asText())),
                        owner.get("nodeGeneration").asLong(),
                        owner.get("ownerId").asText(),
                        owner.get("leaseGeneration").asLong());
        var retired =
                new ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                        local.sourceNodeRid(),
                        local.sourceNodeGeneration(),
                        local.ownerId(),
                        local.ownerLeaseGeneration(),
                        RoutingId.from(HEX.parseHex(session.get("sessionRidHex").asText())),
                        session.get("bindingGeneration").asLong());
        var actorRef =
                new ZLinkBackendActorRef(
                        RoutingId.from(HEX.parseHex(actor.get("ownerNodeRidHex").asText())),
                        actor.get("actorId").asText(),
                        actor.get("objectGeneration").asLong());
        var codec = new ZLinkServiceM6BWireCodec();

        // The Actor owner encodes the stored identity unchanged.
        assertArrayEquals(
                HEX.parseHex(fixture.get("accepted").get("command51Hex").asText()),
                codec.encodeBoundSessionReplaced(
                        new ZLinkServiceM6BWireCodec.BoundSessionReplaced(
                                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                        actorRef,
                                        actor.get("ownerNodeGeneration").asLong(),
                                        actor.get("authorityOwnerGeneration").asLong(),
                                        actor.get("ownerLeaseGeneration").asLong()),
                                retired)));
        assertArrayEquals(
                HEX.parseHex(fixture.get("accepted").get("command54Hex").asText()),
                codec.encodeBoundSessionClose(
                        new ZLinkServiceM6BWireCodec.BoundSessionClose(
                                fixture.get("correlation").asLong(),
                                actorRef.actorId(),
                                actorRef.generation(),
                                retired)));

        // The Session owner accepts only an exact match with its current values.
        assertTrue(identifies(local, codec, fixture.get("accepted"), "command51Hex"));
        assertTrue(identifies(local, codec, fixture.get("accepted"), "command54Hex"));
        for (JsonNode rejected : fixture.get("rejected")) {
            assertFalse(
                    identifies(local, codec, rejected, "command51Hex"),
                    rejected.get("name").asText());
            assertFalse(
                    identifies(local, codec, rejected, "command54Hex"),
                    rejected.get("name").asText());
        }
    }

    @Test
    void aMissingSessionOwnerDescriptorTravelsAsUnavailable() throws Exception {
        JsonNode unavailable = fixture().get("descriptorUnavailable");
        int terminal = Integer.parseInt(unavailable.get("terminalResult").asText());
        int code = Integer.parseInt(unavailable.get("failureCode").asText());
        assertEquals("Unavailable", unavailable.get("errorKind").asText());
        assertArrayEquals(
                new int[] {terminal, code},
                ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.UNAVAILABLE));
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                ZLinkBackendRequestResult.fromWireTerminal(terminal).toFrameworkErrorKind(code));
    }

    private static JsonNode fixture() throws Exception {
        return new ObjectMapper()
                .readTree(
                        Path.of("../../../runtime/protocol/fixtures/session-owner-identity-v1.json")
                                .toFile());
    }

    private static boolean identifies(
            ZLinkInternalMeshNode.PeerAuthorityFence local,
            ZLinkServiceM6BWireCodec codec,
            JsonNode commands,
            String field) {
        byte[] frame = HEX.parseHex(commands.get(field).asText());
        ZLinkServiceM6BWireCodec.RetiredSessionRouteFence fence =
                field.equals("command51Hex")
                        ? codec.decodeBoundSessionReplaced(frame).retiredSession()
                        : codec.decodeBoundSessionClose(frame).session();
        return local.identifies(
                fence.sessionOwnerNodeRid(),
                fence.sessionOwnerNodeGeneration(),
                fence.sessionOwnerId(),
                fence.sessionOwnerLeaseGeneration());
    }
}
