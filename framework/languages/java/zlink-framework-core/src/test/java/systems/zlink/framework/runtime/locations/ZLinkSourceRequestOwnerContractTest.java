package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.TestFactory;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;

import java.lang.reflect.Proxy;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Cross-language source request owner contract (fixture
 * framework/runtime/protocol/fixtures/source-request-owner.json): the first accepting owner reads
 * OwnerId and LeaseGeneration from the admitted peer's descriptor, from the polled descriptors
 * first and the Store once otherwise, and ends the request Unavailable without a live descriptor.
 */
final class ZLinkSourceRequestOwnerContractTest {
    private static final Duration POLLING_INTERVAL = Duration.ofSeconds(10);
    private static final Duration LEASE_TTL = Duration.ofSeconds(1);
    private static final Instant STORE_NOW = Instant.parse("2026-10-19T00:00:00Z");

    @TestFactory
    List<DynamicTest> sourceRequestOwnerCases() throws Exception {
        JsonNode fixture =
                new ObjectMapper()
                        .readTree(
                                Path.of(
                                                "../../../runtime/protocol/fixtures/source-request-owner.json")
                                        .toFile());
        List<DynamicTest> tests = new ArrayList<>();
        for (JsonNode scenario : fixture.get("cases")) {
            tests.add(
                    DynamicTest.dynamicTest(
                            "SourceRequestOwner_" + scenario.get("name").asText(),
                            () -> assertCase(fixture, scenario)));
        }
        return tests;
    }

    private static void assertCase(JsonNode fixture, JsonNode scenario) throws Exception {
        String meshName = fixture.get("meshName").asText();
        RoutingId rid = RoutingId.from(fixture.get("peer").get("nodeRid").asText());
        long nodeGeneration = fixture.get("peer").get("nodeGeneration").asLong();
        String ownerId = fixture.get("descriptor").get("ownerId").asText();
        long leaseGeneration = fixture.get("descriptor").get("leaseGeneration").asLong();
        boolean stored = scenario.get("stored").asBoolean();
        boolean polled = scenario.get("polled").asBoolean();
        boolean leaseLive = scenario.get("leaseLive").asBoolean();

        var clock = new AtomicLong(1);
        var descriptorReads = new AtomicInteger();
        ZLinkMeshNodeDescriptor descriptor =
                descriptor(meshName, rid, nodeGeneration, ownerId, leaseGeneration);
        ZLinkLocationRepository store =
                repository(
                        stored ? List.of(descriptor) : List.of(),
                        new ZLinkLocationOwnerToken(ownerId, leaseGeneration),
                        descriptorReads);
        var resolver = new ZLinkMeshPeerAuthorityResolver(store, POLLING_INTERVAL, clock::get);
        if (polled) {
            assertTrue(resolve(resolver, meshName, rid, nodeGeneration).isPresent());
            descriptorReads.set(0);
        }
        if (!leaseLive) {
            clock.addAndGet(LEASE_TTL.multipliedBy(2).toNanos());
        }

        Optional<ZLinkInternalMeshNode.PeerAuthorityFence> recorded =
                resolve(resolver, meshName, rid, nodeGeneration);

        assertEquals(scenario.get("storeDescriptorReads").asInt(), descriptorReads.get());
        if ("accepted".equals(scenario.get("result").asText())) {
            assertEquals(
                    Optional.of(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                    rid, nodeGeneration, ownerId, leaseGeneration)),
                    recorded);
        } else {
            assertEquals(Optional.empty(), recorded);
        }
    }

    private static Optional<ZLinkInternalMeshNode.PeerAuthorityFence> resolve(
            ZLinkMeshPeerAuthorityResolver resolver,
            String meshName,
            RoutingId rid,
            long nodeGeneration)
            throws Exception {
        return resolver.resolve(meshName, rid, nodeGeneration)
                .toCompletableFuture()
                .get(5, TimeUnit.SECONDS);
    }

    private static ZLinkLocationRepository repository(
            List<ZLinkMeshNodeDescriptor> descriptors,
            ZLinkLocationOwnerToken lease,
            AtomicInteger descriptorReads) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "listMeshNodes" -> {
                                        descriptorReads.incrementAndGet();
                                        yield CompletableFuture.completedFuture(
                                                new ZLinkLocationPage<>(descriptors, null));
                                    }
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    lease.ownerId().equals(args[0])
                                                            ? new ZLinkOwnerLeaseFound(
                                                                    lease,
                                                                    STORE_NOW.plus(LEASE_TTL),
                                                                    STORE_NOW)
                                                            : new ZLinkOwnerLeaseMissing());
                                    case "toString" -> "source-request-owner-store";
                                    default ->
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                });
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            String meshName,
            RoutingId rid,
            long nodeGeneration,
            String ownerId,
            long leaseGeneration) {
        return new ZLinkMeshNodeDescriptor(
                meshName,
                rid,
                nodeGeneration,
                1,
                "tcp://127.0.0.1:7000",
                Map.of(),
                1,
                List.of(),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.empty(),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 1),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                ownerId,
                leaseGeneration,
                STORE_NOW);
    }
}
