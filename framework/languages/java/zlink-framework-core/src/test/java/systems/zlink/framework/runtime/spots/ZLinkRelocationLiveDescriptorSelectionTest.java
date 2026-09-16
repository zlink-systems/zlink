package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.stream.Stream;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.testing.ZLinkDescriptorLeaseTestFixture;
import systems.zlink.framework.testing.ZLinkDescriptorLeaseTestFixture.LeaseState;

final class ZLinkRelocationLiveDescriptorSelectionTest {
    private static final String MESH = "mesh";
    private static final ZLinkStoreCancellation NEVER = () -> false;

    @ParameterizedTest(name = "{0}: expired owner is excluded")
    @MethodSource("descriptorSources")
    void relocationSkipsExpiredDescriptorAndSelectsTheLiveOwner(
        String ignored,
        DescriptorSource source) {
        ZLinkMeshNodeDescriptor expired =
            ZLinkDescriptorLeaseTestFixture.descriptor(
                "expired-target", "inproc://expired", "expired-owner");
        ZLinkMeshNodeDescriptor live =
            ZLinkDescriptorLeaseTestFixture.descriptor(
                "live-target", "inproc://live", "live-owner");

        ZLinkMeshNodeDescriptor selected = select(source.load(
            ZLinkDescriptorLeaseTestFixture.resolver(
                List.of(expired, live),
                Map.of(
                    "expired-owner", LeaseState.EXPIRED,
                    "live-owner", LeaseState.LIVE)))
            .toCompletableFuture().join());

        assertSame(live, selected);
    }

    @ParameterizedTest(name = "{0}: live owner remains eligible")
    @MethodSource("descriptorSources")
    void relocationKeepsLiveDescriptorEligible(
        String ignored,
        DescriptorSource source) {
        ZLinkMeshNodeDescriptor live =
            ZLinkDescriptorLeaseTestFixture.descriptor(
                "live-target", "inproc://live", "live-owner");

        ZLinkMeshNodeDescriptor selected = select(source.load(
            ZLinkDescriptorLeaseTestFixture.resolver(
                List.of(live),
                Map.of("live-owner", LeaseState.LIVE)))
            .toCompletableFuture().join());

        assertSame(live, selected);
    }

    @ParameterizedTest(name = "{0}: missing expiry aborts selection")
    @MethodSource("descriptorSources")
    void relocationFailsWhenLeaseExpiryIsMissing(
        String ignored,
        DescriptorSource source) {
        ZLinkMeshNodeDescriptor corrupt =
            ZLinkDescriptorLeaseTestFixture.descriptor(
                "corrupt-target", "inproc://corrupt", "corrupt-owner");
        ZLinkMeshNodeDescriptor live =
            ZLinkDescriptorLeaseTestFixture.descriptor(
                "live-target", "inproc://live", "live-owner");

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> source.load(ZLinkDescriptorLeaseTestFixture.resolver(
                    List.of(live, corrupt),
                    Map.of(
                        "live-owner", LeaseState.LIVE,
                        "corrupt-owner", LeaseState.MISSING_EXPIRY)))
                .toCompletableFuture().join());

        assertEquals(
            "Location Store owner lease record is invalid",
            failure.getCause().getMessage());
    }

    private static Stream<Arguments> descriptorSources() {
        return Stream.of(
            Arguments.of(
                "Actor relocation",
                (DescriptorSource) resolver ->
                    ZLinkStandaloneActorRelocationSourceBuilder
                        .listDescriptors(resolver, MESH, NEVER)),
            Arguments.of(
                "User Spot retire relocation",
                (DescriptorSource) resolver ->
                    ZLinkUserSpotRetireSourceBuilder
                        .listDescriptors(resolver, MESH, NEVER)));
    }

    private static ZLinkMeshNodeDescriptor select(
        List<ZLinkMeshNodeDescriptor> descriptors) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            new ZLinkRelocationTargetPolicy(
                ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
                7,
                Optional.empty(),
                9),
            ignored -> true,
            ignored -> true,
            ignored -> true,
            "no live relocation target");
    }

    @FunctionalInterface
    private interface DescriptorSource {
        CompletionStage<List<ZLinkMeshNodeDescriptor>> load(
            ZLinkStoreLocationResolvers resolver);
    }
}
