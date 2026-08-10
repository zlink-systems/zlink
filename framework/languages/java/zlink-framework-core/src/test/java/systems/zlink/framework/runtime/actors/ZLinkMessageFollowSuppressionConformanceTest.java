package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.spots.ZLinkSpotKind;

final class ZLinkMessageFollowSuppressionConformanceTest {
    @Test
    void consumesEverySharedSuppressionScenario() throws Exception {
        JsonNode fixture = fixture();
        assertEquals(
            "zlink.framework.message-follow-suppression",
            fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());
        assertEquals(7, fixture.path("routeFenceFields").size());

        Map<String, ZLinkMessageFollowSuppressionRegistry.Key> keys = new HashMap<>();
        fixture.path("keys").fields().forEachRemaining(entry ->
            keys.put(entry.getKey(), key(entry.getValue())));
        assertNotEquals(keys.get("base"), keys.get("changedTargetLease"));
        assertNotEquals(keys.get("base"), keys.get("differentMulticastTarget"));
        assertEquals(11, fixture.path("independentKeyMutations").size());

        for (JsonNode scenario : fixture.path("scenarios")) {
            ZLinkMessageFollowSuppressionRegistry registry =
                new ZLinkMessageFollowSuppressionRegistry();
            Map<String, ZLinkMessageFollowSuppressionRegistry.Claim> claims =
                new HashMap<>();
            for (JsonNode operation : scenario.path("operations")) {
                String kind = operation.path("kind").asText();
                String keyName = operation.path("key").asText();
                ZLinkMessageFollowSuppressionRegistry.Key selected = keys.get(keyName);
                switch (kind) {
                    case "begin" -> {
                        var claim = registry.begin(selected);
                        boolean expectedGranted =
                            "granted".equals(operation.path("result").asText());
                        assertEquals(expectedGranted, claim.isPresent(), scenario.path("name").asText());
                        if (claim.isPresent()) {
                            claims.put(operation.path("claim").asText(), claim.orElseThrow());
                        }
                    }
                    case "markSent" -> assertEquals(
                        "applied".equals(operation.path("result").asText()),
                        registry.markSent(claims.get(operation.path("claim").asText())));
                    case "abort" -> assertEquals(
                        "applied".equals(operation.path("result").asText()),
                        registry.abort(claims.get(operation.path("claim").asText())));
                    case "expireRoute" -> assertTrue(registry.expire(selected));
                    case "retainRoute" -> {
                        // A newly retained route starts without a suppression marker.
                    }
                    case "replaceRoute" -> assertTrue(registry.expire(selected));
                    default -> throw new AssertionError("unknown fixture operation: " + kind);
                }
                assertEquals(
                    state(operation.path("state").asText()),
                    registry.state("replaceRoute".equals(kind)
                        ? keys.get(operation.path("replacement").asText())
                        : selected),
                    scenario.path("name").asText() + ":" + kind);
            }
        }
    }

    @Test
    void productionNoticeCompletionRestoresFailureAndSuppressesSuccess() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute = route(
            "source", 2, 41, 51);
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute = route(
            "target", 3, 42, 52);
        SpotTransportAddress targetAddress = new SpotTransportAddress(
            "router", RoutingId.from("target"), "spot", 31, 3, 42, 52,
            ZLinkSpotKind.USER);
        handoff.retain(
            "actor-17",
            new ZLinkBackendActorRef(RoutingId.from("source"), "actor-17", 31),
            new ZLinkBackendActorRef(RoutingId.from("target"), "actor-17", 31),
            targetAddress,
            targetRoute,
            Duration.ofMinutes(1),
            ignored -> { });
        try {
            ZLinkActorTransferHandoff.MessageFollowSource source =
                handoff.messageFollowSource("actor-17").orElseThrow();
            var failed = source.beginMessageFollowNotice(sourceRoute).orElseThrow();
            ZLinkActorRuntime.completeMessageFollowNotice(
                source, failed, new IllegalStateException("rejected"));

            var retry = source.beginMessageFollowNotice(sourceRoute).orElseThrow();
            ZLinkActorRuntime.completeMessageFollowNotice(source, retry, null);
            assertTrue(source.beginMessageFollowNotice(sourceRoute).isEmpty());

            source.abortMessageFollowNotice(failed);
            assertTrue(source.beginMessageFollowNotice(sourceRoute).isEmpty(),
                "a stale claim must not reopen the sent marker");
        } finally {
            handoff.close();
        }
    }

    @Test
    void routeReplacementAndExpiryRemoveOwnedMarkers() throws Exception {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute = route(
            "source", 2, 41, 51);
        ZLinkServiceMessageFollowWireCodec.ActorRoute firstTarget = route(
            "target", 3, 42, 52);
        ZLinkActorTransferHandoff.MessageFollowSource first = retain(
            handoff, firstTarget, Duration.ofMinutes(1));
        var firstClaim = first.beginMessageFollowNotice(sourceRoute).orElseThrow();
        first.markMessageFollowNoticeSent(firstClaim);

        ZLinkServiceMessageFollowWireCodec.ActorRoute replacement = route(
            "target", 3, 42, 53);
        ZLinkActorTransferHandoff.MessageFollowSource second = retain(
            handoff, replacement, Duration.ofMillis(20));
        assertEquals(0, handoff.messageFollowSuppressionCount(),
            "replacement must remove the old route marker");
        assertTrue(first.beginMessageFollowNotice(sourceRoute).isEmpty());
        var secondClaim = second.beginMessageFollowNotice(sourceRoute).orElseThrow();
        second.markMessageFollowNoticeSent(secondClaim);
        Thread.sleep(100);
        assertFalse(handoff.messageFollowSource("actor-17").isPresent());
        assertEquals(0, handoff.messageFollowSuppressionCount(),
            "expiry must remove the retained route marker");
        assertTrue(second.beginMessageFollowNotice(sourceRoute).isEmpty());
        handoff.close();
    }

    private static ZLinkActorTransferHandoff.MessageFollowSource retain(
        ZLinkActorTransferHandoff handoff,
        ZLinkServiceMessageFollowWireCodec.ActorRoute target,
        Duration duration) {
        SpotTransportAddress address = new SpotTransportAddress(
            "router", target.targetNodeRid(), "spot", 31,
            target.targetNodeGeneration(), target.authorityOwnerGeneration(),
            target.ownerLeaseGeneration(), ZLinkSpotKind.USER);
        handoff.retain(
            "actor-17",
            new ZLinkBackendActorRef(RoutingId.from("source"), "actor-17", 31),
            new ZLinkBackendActorRef(target.targetNodeRid(), "actor-17", 31),
            address,
            target,
            duration,
            ignored -> { });
        return handoff.messageFollowSource("actor-17").orElseThrow();
    }

    private static ZLinkServiceMessageFollowWireCodec.ActorRoute route(
        String node,
        long nodeGeneration,
        long authorityGeneration,
        long leaseGeneration) {
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            "actor-17", 31, RoutingId.from(node), nodeGeneration,
            authorityGeneration, leaseGeneration);
    }

    private static ZLinkMessageFollowSuppressionRegistry.Key key(JsonNode pair) {
        return new ZLinkMessageFollowSuppressionRegistry.Key(
            fence(pair.path("sourceRoute")),
            fence(pair.path("targetRoute")));
    }

    private static ZLinkMessageFollowSuppressionRegistry.RouteFence fence(JsonNode route) {
        return new ZLinkMessageFollowSuppressionRegistry.RouteFence(
            route.path("objectKind").asText(),
            route.path("logicalObjectId").asText(),
            Long.parseLong(route.path("objectGeneration").asText()),
            route.path("targetNodeRid").asText(),
            Long.parseLong(route.path("targetNodeGeneration").asText()),
            Long.parseLong(route.path("authorityOwnerGeneration").asText()),
            Long.parseLong(route.path("ownerLeaseGeneration").asText()));
    }

    private static ZLinkMessageFollowSuppressionRegistry.State state(String state) {
        return switch (state) {
            case "idle" -> ZLinkMessageFollowSuppressionRegistry.State.IDLE;
            case "inFlight" -> ZLinkMessageFollowSuppressionRegistry.State.IN_FLIGHT;
            case "sentUntilExpiry" ->
                ZLinkMessageFollowSuppressionRegistry.State.SENT_UNTIL_EXPIRY;
            default -> throw new AssertionError("unknown fixture state: " + state);
        };
    }

    private static JsonNode fixture() throws Exception {
        return new ObjectMapper().readTree(Files.readString(sharedFixture()));
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/conformance/message-follow-suppression-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared Message Follow suppression fixture was not found");
    }
}
