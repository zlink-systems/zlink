package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.TestFactory;

import java.util.stream.Stream;
import java.util.stream.StreamSupport;

final class ZLinkAuthorityAggregateRelocationGoldenTest {
    private static final ObjectMapper JSON = new ObjectMapper();

    @TestFactory
    Stream<DynamicTest> canonicalProviderReaderAndWriter() throws Exception {
        var input = getClass().getResourceAsStream("/authority-aggregate-relocation-v1.json");
        assertNotNull(input);
        JsonNode cases;
        try (input) {
            cases = JSON.readTree(input).path("cases");
        }
        return StreamSupport.stream(cases.spliterator(), false)
                .map(
                        c ->
                                DynamicTest.dynamicTest(
                                        c.path("name").asText(),
                                        () -> {
                                            ObjectNode record = JSON.createObjectNode();
                                            record.put("recordVersion", 1);
                                            record.put("payload", "AQID");
                                            record.put("objectGeneration", "1");
                                            record.put("authorityOwnerGeneration", "2");
                                            record.put("ownerId", "owner-src");
                                            record.put("ownerLeaseGeneration", "7");
                                            record.set(
                                                    "allocation",
                                                    JSON.readTree(
                                                            """
                    {"state":"active","objectKind":"actor","stableType":"actor",
                     "descriptor":{"meshName":"mesh","routingIdHex":"0a0b"},
                     "descriptorLifecycleGeneration":"3",
                     "capacity":{"actors":1,"spots":0,"spotType":null}}
                    """));
                                            record.putNull("pendingCreation");
                                            record.set("aggregate", c.path("aggregate"));
                                            if (c.path("aggregate").has("aggregateId"))
                                                record.put("visibleStoreVersion", "opaque-v1");
                                            if (c.path("expect").asText().equals("reject")) {
                                                assertThrows(
                                                        IllegalStateException.class,
                                                        () ->
                                                                ZLinkProviderAuthorityRepository
                                                                        .decode(
                                                                                JSON
                                                                                        .writeValueAsBytes(
                                                                                                record)));
                                                return;
                                            }
                                            var decoded =
                                                    ZLinkProviderAuthorityRepository.decode(
                                                            JSON.writeValueAsBytes(record));
                                            JsonNode rewritten =
                                                    JSON.readTree(
                                                            ZLinkProviderAuthorityRepository.encode(
                                                                    decoded));
                                            if (decoded.relocation() != null) {
                                                long markerGeneration =
                                                        c.path("aggregate")
                                                                        .has("aggregateGeneration")
                                                                ? Long.parseUnsignedLong(
                                                                        c.path("aggregate")
                                                                                .path(
                                                                                        "aggregateGeneration")
                                                                                .asText())
                                                                : 0;
                                                var snapshot =
                                                        new ZLinkAuthoritySnapshot(
                                                                "current-v2",
                                                                decoded.payload(),
                                                                decoded.objectGeneration(),
                                                                decoded.authorityOwnerGeneration(),
                                                                decoded.ownerId(),
                                                                decoded.ownerLeaseGeneration(),
                                                                decoded.allocation(),
                                                                decoded.pendingCreation(),
                                                                java.time.Instant.EPOCH,
                                                                decoded.relocation(),
                                                                markerGeneration);
                                                var view =
                                                        ZLinkAuthorityRelocation.publication(
                                                                snapshot, 77);
                                                assertEquals(
                                                        decoded.relocation().relocationId(),
                                                        view.aggregateId());
                                                assertEquals(
                                                        markerGeneration == 0
                                                                ? 77
                                                                : markerGeneration,
                                                        view.aggregateGeneration());
                                                assertEquals(
                                                        "opaque-v1",
                                                        view.coordinatorExpectedStoreVersion());
                                                assertArrayEquals(
                                                        decoded.payload(),
                                                        view.applicationPayload());
                                                assertNull(
                                                        ZLinkCanonicalRelocationAuthorityStateCodec
                                                                .decode(snapshot.payload()));
                                            }

                                            assertEquals(
                                                    c.path("aggregate"),
                                                    rewritten.path("aggregate"));
                                            assertEquals(
                                                    record.path("payload"),
                                                    rewritten.path("payload"));
                                        }));
    }
}
