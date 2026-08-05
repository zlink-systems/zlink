package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

final class ZLinkRelocationTargetSelectorTest {
    @Test
    void plannedMaintenanceChecksVersionThenCapabilityThenDifferentWave() {
        AtomicInteger capabilityChecks = new AtomicInteger();
        AtomicInteger capacityChecks = new AtomicInteger();
        var policy = new ZLinkRelocationTargetPolicy(
            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
            7,
            Optional.of("blue"),
            7);

        assertThrows(
            IllegalStateException.class,
            () -> select(
                descriptor(8, Optional.of("green")),
                policy,
                capabilityChecks,
                capacityChecks,
                true,
                true));
        assertEquals(0, capabilityChecks.get());
        assertEquals(0, capacityChecks.get());

        assertThrows(
            IllegalStateException.class,
            () -> select(
                descriptor(7, Optional.of("blue")),
                policy,
                capabilityChecks,
                capacityChecks,
                true,
                true));
        assertEquals(1, capabilityChecks.get());
        assertEquals(0, capacityChecks.get());
    }

    @Test
    void plannedMaintenanceDoesNotRequireWaveWhenSourceHasNone() {
        var policy = new ZLinkRelocationTargetPolicy(
            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
            7,
            Optional.empty(),
            7);

        ZLinkMeshNodeDescriptor candidate = descriptor(7, Optional.empty());
        assertSame(
            candidate,
            select(
                candidate,
                policy,
                new AtomicInteger(),
                new AtomicInteger(),
                true,
                true));
    }

    @Test
    void plannedMaintenanceAcceptsMissingCandidateWaveWhenSourceHasWave() {
        var policy = new ZLinkRelocationTargetPolicy(
            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
            7,
            Optional.of("blue"),
            7);

        ZLinkMeshNodeDescriptor candidate = descriptor(7, Optional.empty());
        assertSame(
            candidate,
            select(
                candidate,
                policy,
                new AtomicInteger(),
                new AtomicInteger(),
                true,
                true));
    }

    @Test
    void rollingUpdateUsesExactRequestedVersionThenCapabilityThenCapacity() {
        AtomicInteger capabilityChecks = new AtomicInteger();
        AtomicInteger capacityChecks = new AtomicInteger();
        var policy = new ZLinkRelocationTargetPolicy(
            ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
            7,
            Optional.of("blue"),
            9);

        assertThrows(
            IllegalStateException.class,
            () -> select(
                descriptor(9, Optional.of("blue")),
                policy,
                capabilityChecks,
                capacityChecks,
                false,
                true));
        assertEquals(1, capabilityChecks.get());
        assertEquals(0, capacityChecks.get());

        ZLinkMeshNodeDescriptor accepted =
            descriptor(9, Optional.of("green"));
        assertSame(
            accepted,
            select(
                accepted,
                policy,
                capabilityChecks,
                capacityChecks,
                true,
                true));
        assertEquals(2, capabilityChecks.get());
        assertEquals(1, capacityChecks.get());
    }

    @Test
    void weightIsCheckedOnlyAfterCapacity() {
        AtomicInteger capabilityChecks = new AtomicInteger();
        AtomicInteger capacityChecks = new AtomicInteger();
        var policy = new ZLinkRelocationTargetPolicy(
            ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
            7,
            Optional.empty(),
            9);

        assertThrows(
            IllegalStateException.class,
            () -> select(
                descriptor(9, Optional.empty(), 0),
                policy,
                capabilityChecks,
                capacityChecks,
                true,
                true));
        assertEquals(1, capabilityChecks.get());
        assertEquals(1, capacityChecks.get());
    }

    private static ZLinkMeshNodeDescriptor select(
        ZLinkMeshNodeDescriptor candidate,
        ZLinkRelocationTargetPolicy policy,
        AtomicInteger capabilityChecks,
        AtomicInteger capacityChecks,
        boolean capability,
        boolean capacity) {
        return ZLinkRelocationTargetSelector.select(
            List.of(candidate),
            policy,
            ignored -> true,
            ignored -> {
                capabilityChecks.incrementAndGet();
                return capability;
            },
            ignored -> {
                capacityChecks.incrementAndGet();
                return capacity;
            },
            "unavailable");
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        long applicationVersion,
        Optional<String> maintenanceWave) {
        return descriptor(applicationVersion, maintenanceWave, 100);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        long applicationVersion,
        Optional<String> maintenanceWave,
        int placementWeight) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            RoutingId.from("target"),
            1,
            1,
            "inproc://target",
            Map.of(),
            applicationVersion,
            List.of(),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(
                "target-entry-00000000-0000-4000-8000-000000000001"),
            placementWeight,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 1),
            maintenanceWave,
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            "owner",
            1,
            Instant.now());
    }
}
