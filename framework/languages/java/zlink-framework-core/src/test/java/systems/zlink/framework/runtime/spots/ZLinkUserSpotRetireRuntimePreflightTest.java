package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

final class ZLinkUserSpotRetireRuntimePreflightTest {
    @Test
    void laterTargetFailurePreventsEveryRelocationUnitFromStarting() {
        var events = new ArrayList<String>();
        var stateTransitions = new AtomicInteger();
        var relocationStarts = new AtomicInteger();
        var plan = new ZLinkUserSpotRetireRuntime.RelocationPlan(
            List.of("spot-a", "spot-b"),
            List.of("actor-c"));

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> ZLinkUserSpotRetireRuntime.executePlan(
                plan,
                spotId -> {
                    events.add("preflight:" + spotId);
                    return spotId.equals("spot-b")
                        ? CompletableFuture.failedFuture(
                            new ZLinkUserSpotRetireRuntime
                                .RelocationBlockedException(
                                    ZLinkFrameworkRelocationReason
                                        .TARGET_UNAVAILABLE,
                                    "no target"))
                        : CompletableFuture.completedFuture(null);
                },
                actorId -> {
                    events.add("preflight:" + actorId);
                    return CompletableFuture.completedFuture(null);
                },
                stateTransitions::incrementAndGet,
                () -> false,
                spotId -> {
                    relocationStarts.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                },
                actorId -> {
                    relocationStarts.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }).toCompletableFuture().join());

        assertEquals(
            ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
            ((ZLinkUserSpotRetireRuntime.RelocationBlockedException)
                failure.getCause()).reason());
        assertEquals(
            List.of("preflight:spot-a", "preflight:spot-b"),
            events);
        assertEquals(0, stateTransitions.get());
        assertEquals(0, relocationStarts.get());
    }

    @Test
    void allUnitsPassPreflightBeforeHostStateOrRelocationChanges() {
        var events = new ArrayList<String>();
        var plan = new ZLinkUserSpotRetireRuntime.RelocationPlan(
            List.of("spot-a", "spot-b"),
            List.of("actor-c"));

        ZLinkUserSpotRetireRuntime.executePlan(
            plan,
            spotId -> {
                events.add("preflight:" + spotId);
                return CompletableFuture.completedFuture(null);
            },
            actorId -> {
                events.add("preflight:" + actorId);
                return CompletableFuture.completedFuture(null);
            },
            () -> events.add("host-state"),
            () -> false,
            spotId -> {
                events.add("relocate:" + spotId);
                return CompletableFuture.completedFuture(null);
            },
            actorId -> {
                events.add("relocate:" + actorId);
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture().join();

        assertEquals(List.of(
            "preflight:spot-a",
            "preflight:spot-b",
            "preflight:actor-c",
            "host-state",
            "relocate:spot-a",
            "relocate:spot-b",
            "relocate:actor-c"), events);
    }

    @Test
    void relocationWaitsForAwaitedHostAdmissionPublication() {
        var events = new ArrayList<String>();
        var publication = new CompletableFuture<Void>();
        var completion = ZLinkUserSpotRetireRuntime.executePlan(
            new ZLinkUserSpotRetireRuntime.RelocationPlan(
                List.of("spot-a"), List.of()),
            spotId -> CompletableFuture.completedFuture(null),
            actorId -> CompletableFuture.completedFuture(null),
            () -> {
                events.add("publication-started");
                return publication;
            },
            () -> false,
            spotId -> {
                events.add("relocation-started");
                return CompletableFuture.completedFuture(null);
            },
            actorId -> CompletableFuture.completedFuture(null));

        assertEquals(List.of("publication-started"), events);
        assertEquals(false, completion.toCompletableFuture().isDone());

        publication.complete(null);
        completion.toCompletableFuture().join();
        assertEquals(
            List.of("publication-started", "relocation-started"),
            events);
    }

    @Test
    void readyUnitsRelocateWithoutWaitingForAnEarlierUnitToFinish() {
        var first = new CompletableFuture<Void>();
        var events = new ArrayList<String>();
        var completion = ZLinkUserSpotRetireRuntime.executePlan(
            new ZLinkUserSpotRetireRuntime.RelocationPlan(
                List.of("spot-a", "spot-b"), List.of("actor-c")),
            spotId -> CompletableFuture.completedFuture(null),
            actorId -> CompletableFuture.completedFuture(null),
            () -> {
            },
            () -> false,
            spotId -> {
                events.add("start:" + spotId);
                return spotId.equals("spot-a")
                    ? first
                    : CompletableFuture.completedFuture(null);
            },
            actorId -> {
                events.add("start:" + actorId);
                return CompletableFuture.completedFuture(null);
            });

        assertEquals(
            List.of("start:spot-a", "start:spot-b", "start:actor-c"),
            events);
        assertEquals(false, completion.toCompletableFuture().isDone());

        first.complete(null);
        completion.toCompletableFuture().join();
    }

    @Test
    void hostPreflightAccountsForCapacityAcrossAllKnownUnits() {
        var capacity = new ZLinkRelocationCapacityPlan();
        ZLinkMeshNodeDescriptor target = targetWithCapacity(2, 1);

        assertEquals(
            true,
            capacity.canReserveUserSpot(target, "room", 1));
        capacity.reserveUserSpot("room-1", target, "room", 1);

        assertEquals(
            false,
            capacity.canReserveUserSpot(target, "room", 0));
        assertEquals(true, capacity.canReserveActor(target));
        capacity.reserveActor("actor-1", target);
        assertEquals(false, capacity.canReserveActor(target));
    }

    private static ZLinkMeshNodeDescriptor targetWithCapacity(
        int actorLimit,
        int spotLimit) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            RoutingId.from("target"),
            1,
            1,
            "inproc://target",
            Map.of(),
            2,
            List.of(),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(
                "target-entry-00000000-0000-4000-8000-000000000001"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, actorLimit),
                new ZLinkCapacityUsage(0, 0, spotLimit),
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "room",
                    new ZLinkCapacityUsage(0, 0, spotLimit)))),
            new ZLinkActivationConcurrency(0, 1),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            "owner",
            1,
            java.time.Instant.now());
    }

    //  BLK-043 acceptance. The spec unit gate of 28-graceful-drain-handoff §7
    //  belongs to ZLinkRelocationPermitPool, which admits at the turn boundary
    //  where the actual payload is known. executePlan must therefore not bound
    //  the units a second time: a unit waiting on its own turn boundary must
    //  not hold a slot that a later unit needs.
    @Test
    void executePlanStartsEveryUnitWithoutASecondConcurrencyBound() {
        var held = new ArrayList<CompletableFuture<Void>>();
        var started = new ArrayList<String>();
        var spotIds = new ArrayList<String>();
        for (int index = 0; index < 64; index++) {
            spotIds.add("waiting-" + index);
        }
        spotIds.add("ready");

        var completion = ZLinkUserSpotRetireRuntime.executePlan(
            new ZLinkUserSpotRetireRuntime.RelocationPlan(
                List.copyOf(spotIds), List.of()),
            spotId -> CompletableFuture.completedFuture(null),
            actorId -> CompletableFuture.completedFuture(null),
            () -> {
            },
            () -> false,
            spotId -> {
                started.add(spotId);
                if (spotId.equals("ready")) {
                    return CompletableFuture.completedFuture(null);
                }
                var turnBoundary = new CompletableFuture<Void>();
                held.add(turnBoundary);
                return turnBoundary;
            },
            actorId -> CompletableFuture.completedFuture(null));

        assertEquals(
            spotIds.size(),
            started.size(),
            "every unit must reach its own admission point; "
                + "started=" + started);
        assertEquals(false, completion.toCompletableFuture().isDone());

        held.forEach(turnBoundary -> turnBoundary.complete(null));
        completion.toCompletableFuture().join();
    }

    //  A unit that fails does not strand the others. Every unit still reaches a
    //  terminal state and the first failure is the reported one.
    @Test
    void executePlanReportsTheFirstFailureAfterEveryUnitSettles() {
        var second = new CompletableFuture<Void>();
        var settled = new ArrayList<String>();

        var completion = ZLinkUserSpotRetireRuntime.executePlan(
            new ZLinkUserSpotRetireRuntime.RelocationPlan(
                List.of("spot-a", "spot-b"), List.of()),
            spotId -> CompletableFuture.completedFuture(null),
            actorId -> CompletableFuture.completedFuture(null),
            () -> {
            },
            () -> false,
            spotId -> {
                settled.add(spotId);
                return spotId.equals("spot-a")
                    ? CompletableFuture.failedFuture(
                        new IllegalStateException("first"))
                    : second;
            },
            actorId -> CompletableFuture.completedFuture(null));

        assertEquals(List.of("spot-a", "spot-b"), settled);
        assertEquals(false, completion.toCompletableFuture().isDone());

        second.complete(null);
        var failure = assertThrows(
            CompletionException.class,
            () -> completion.toCompletableFuture().join());
        assertEquals("first", failure.getCause().getMessage());
    }
}
