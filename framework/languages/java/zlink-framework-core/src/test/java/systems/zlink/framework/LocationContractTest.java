package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateCall;
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.actors.ZLinkActorSendCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;
import systems.zlink.framework.spots.ZLinkSpotSendCall;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkLocationReadiness;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.locations.ZLinkLocationTopologyState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateCall;
import systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall;
import systems.zlink.framework.spots.ZLinkSpotInfo;
import systems.zlink.framework.spots.ZLinkSpotKind;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.streams.ZLinkSessionActors;

final class LocationContractTest {
    @Test
    void frameworkRootDoesNotExposeDirectRuntimeStartFacades() {
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName("systems.zlink.framework.ZLinkFramework"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName("systems.zlink.framework.ZLinkRegistry"));
    }

    @Test
    void runtimeTypeDoesNotExposePublicDirectStartMembers() {
        assertEquals(0, ZLinkFrameworkRuntime.class.getConstructors().length);
        assertFalse(Arrays.stream(ZLinkFrameworkRuntime.class.getMethods())
            .anyMatch(method -> method.getName().equals("start")
                && Modifier.isPublic(method.getModifiers())));
    }

    @Test
    void legacyRegistryAndDiscoveryContractsAreNotPublicSurface() {
        assertMissing("systems.zlink.framework.registry.ZLinkRegistryQuery");
        assertMissing("systems.zlink.framework.registry.ZLinkRegistryQueryClient");
        assertMissing("systems.zlink.framework.runtime.registry.ZLinkRegistryRuntime");
        assertMissing("systems.zlink.framework.runtime.internal.backend.ZLinkBackendDiscovery");
        assertMissing("systems.zlink.framework.runtime.spots.SpotDiscoveryReconciler");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkPeerLocation");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkSpotLocation");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkActorLocation");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkRouteLocation");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkLocationWatchFilter");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkLocationChangeStampScope");
        assertMissing("systems.zlink.framework.runtime.internal.locations.ZLinkLocationKind");
        assertMissing("systems.zlink.framework.monitoring.ZLinkDrainEvent");
        assertMissing("systems.zlink.framework.monitoring.ZLinkDrainState");
    }

    @Test
    void locationStoreOwnsProviderOperationsDirectly() throws Exception {
        Method claimOwnerLease = ZLinkLocationRepository.class.getMethod(
            "claimOwnerLease",
            String.class,
            java.time.Duration.class);
        Method readOwnerLease = ZLinkLocationRepository.class.getMethod(
            "readOwnerLease",
            String.class);
        Method renewOwnerLease = ZLinkLocationRepository.class.getMethod(
            "renewOwnerLease",
            ZLinkLocationOwnerToken.class,
            java.time.Duration.class);
        Method releaseOwnerLease = ZLinkLocationRepository.class.getMethod(
            "releaseOwnerLease",
            ZLinkLocationOwnerToken.class);
        Method removeAllByOwner = ZLinkLocationRepository.class.getMethod(
            "removeAllByOwner",
            ZLinkLocationOwnerToken.class);
        Method updateMeshNode =
            ZLinkLocationRepository.class.getMethod(
                    "updateMeshNode",
                    systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor.class,
                    ZLinkLocationWriteIntent.class);
        Method removeMeshNode =
            ZLinkLocationRepository.class.getMethod(
                    "removeMeshNode",
                    systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey.class,
                    ZLinkLocationOwnerToken.class);
        Method listMeshNodes =
            ZLinkLocationRepository.class.getMethod(
                    "listMeshNodes",
                    String.class,
                    ZLinkPageRequest.class);
        assertEquals(CompletionStage.class, claimOwnerLease.getReturnType());
        assertEquals(CompletionStage.class, readOwnerLease.getReturnType());
        assertEquals(CompletionStage.class, renewOwnerLease.getReturnType());
        assertEquals(CompletionStage.class, releaseOwnerLease.getReturnType());
        assertEquals(CompletionStage.class, removeAllByOwner.getReturnType());
        assertEquals(CompletionStage.class, updateMeshNode.getReturnType());
        assertEquals(CompletionStage.class, removeMeshNode.getReturnType());
        assertEquals(CompletionStage.class, listMeshNodes.getReturnType());
    }

    @Test
    void frameworkOptionsExposeLocationStoreRegistrationSurface() throws Exception {
        assertNoMethod("add" + "Peer" + "LocationStore");
        assertNoMethod("add" + "Spot" + "LocationStore");
        assertNoMethod("add" + "Actor" + "LocationStore");
        assertNoMethod("add" + "Route" + "LocationStore");
        assertNoMethod("add" + "OwnerLease" + "Store");
        assertNoMethod("useInMemoryLocationStores");
        assertEquals(void.class, ZLinkFrameworkOptions.class
            .getMethod(
                "addLocationStore",
                systems.zlink.framework.locationprovider
                    .ZLinkLocationStore.class)
            .getReturnType());
        assertEquals(ZLinkLocationOptions.class, ZLinkFrameworkOptions.class
            .getMethod("configureLocations")
            .getReturnType());
        assertNoPublicMethod(ZLinkLocationOptions.class, "setSpotRouterChannel");
        assertNoPublicMethod(ZLinkLocationOptions.class, "spotRouterChannels");

        assertEquals(ZLinkLocationRepository.class, ZLinkLocationRepository.class);
    }

    @Test
    void locationRuntimeQueryExposesPagedRuntimeViews() throws Exception {
        assertEquals(CompletionStage.class, ZLinkLocationRuntimeQuery.class
            .getMethod("getStatus")
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkLocationRuntimeQuery.class
            .getMethod(
                "listTopology",
                systems.zlink.framework.locations.ZLinkLocationTopologyFilter.class,
                ZLinkPageRequest.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkLocationRuntimeQuery.class
            .getMethod(
                "listServiceSummaries",
                systems.zlink.framework.locations.ZLinkLocationServiceSummaryFilter.class,
                ZLinkPageRequest.class)
            .getReturnType());
        assertEquals(ZLinkLocationPage.class, ZLinkLocationPage.class);
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.locations.ZLinkOwnerLease"));
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.locations.ZLinkOwnerLeaseSnapshot"));
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.locations.ZLinkOwnerLeaseRenewal"));
    }

    @Test
    void s4ConvenienceContractsArePublicAndTyped() throws Exception {
        assertEquals(CompletionStage.class, ZLinkActorDirectory.class
            .getMethod("find", String.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkActorDirectory.class
            .getMethod(
                "ensure",
                String.class,
                systems.zlink.framework.messaging.ZLinkMessage.class)
            .getReturnType());
        assertMissing("systems.zlink.framework.actors.ZLinkActorPlacement");
        assertRecordComponents(
            ActorRef.class,
            "actorId",
            "objectGeneration",
            "meshName",
            "nodeRid");
        assertRecordComponents(
            ActorRefSnapshot.class,
            "actorId",
            "objectGeneration",
            "meshName",
            "nodeRid");
        assertEquals(ActorRef.class, ActorRefSnapshot.class
            .getMethod("toActorRef")
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkLocationReadiness.class
            .getMethod("isPeerReady", String.class, ZLinkLocationRole.class, RoutingId.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkSessionActors.class
            .getMethod("bindOrGet", ActorRef.class)
            .getReturnType());
        assertMissing("systems.zlink.framework.channels.ZLinkRoute" + "RequestCall");
    }

    @Test
    void actorClientPinsGlobalIdCompletionStageSurface() throws Exception {
        assertEquals(ZLinkActorSendCall.class, ZLinkActorClient.class
            .getMethod("sendToActor", String.class, Object.class)
            .getReturnType());
        assertEquals(ZLinkActorRequestCall.class, ZLinkActorClient.class
            .getMethod("requestToActor", String.class, Object.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkActorSendCall.class
            .getMethod("submit")
            .getReturnType());
        assertNoPublicMethod(ZLinkActorSendCall.class, "await");
        assertEquals(ZLinkActorRequestCall.class, ZLinkActorRequestCall.class
            .getMethod("timeout", java.time.Duration.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkActorRequestCall.class
            .getMethod("submit", Class.class)
            .getReturnType());
        assertNoPublicMethod(ZLinkActorRequestCall.class, "await", Class.class);
        assertNoPublicMethod(ZLinkActorSendCall.class, "submit", Class.class);
        assertNoPublicMethod(ZLinkActorRequestCall.class, "submit");
    }

    @Test
    void enumValuesMatchDotnetContractTablesValueByValue() throws Exception {
        assertEnumValues(ZLinkFrameworkErrorKind.class, Map.ofEntries(
            Map.entry("NOT_FOUND", 0),
            Map.entry("ALREADY_EXISTS", 1),
            Map.entry("TYPE_MISMATCH", 2),
            Map.entry("NOT_CONFIGURED", 3),
            Map.entry("REJECTED", 4),
            Map.entry("UNAVAILABLE", 5),
            Map.entry("CAPACITY_EXCEEDED", 6),
            Map.entry("DEADLINE_EXCEEDED", 7),
            Map.entry("SHUTTING_DOWN", 8),
            Map.entry("PROTOCOL_ERROR", 9),
            Map.entry("INVALID_OPERATION", 10),
            Map.entry("DATA_LOST", 11),
            Map.entry("INTERNAL_FAILURE", 12)));

        assertEnumValues(ZLinkLocationRole.class, Map.of(
            "INVALID", 0,
            "SPOT", 2,
            "ROUTER", 3,
            "DEALER", 4,
            "PUB", 5,
            "SUB", 6));
        assertEnumValues(ZLinkLocationWriteIntent.class, Map.of(
            "NEW_CLAIM", 1,
            "RENEW", 2,
            "TAKEOVER", 3));
        assertEnumValues(ZLinkLocationWriteStatus.class, Map.of(
            "STORED", 1,
            "IGNORED_STALE", 2,
            "REJECTED_CONFLICT", 3));
        assertEnumValues(ZLinkLocationTopologyState.class, Map.of(
            "DISCOVERED", 1,
            "CONNECTING", 2,
            "READY", 3,
            "LOST", 4,
            "ERROR", 5,
            "STOPPED", 6));
        assertEnumValues(ZLinkSpotKind.class, Map.of(
            "INVALID", 0,
            "ENTRY", 1,
            "USER", 2,
            "INSTANCE", 3));
    }

    @Test
    void actorJoinAndManagerSurfacesPinSharedCallShape() throws Exception {
        assertEquals(void.class, ZLinkActorJoinCall.class
            .getMethod("defer")
            .getReturnType());
        assertEquals(ZLinkActorJoinCall.class, ZLinkActorJoinCall.class
            .getMethod("timeout", java.time.Duration.class)
            .getReturnType());
        assertTrue(ZLinkActorJoinCompletion.class.isSealed());
        assertRecordComponents(
            ZLinkActorJoinCompletion.Accepted.class,
            "operationId", "actor", "reply");
        assertRecordComponents(
            ZLinkActorJoinCompletion.Rejected.class,
            "operationId", "reply");
        assertRecordComponents(
            ZLinkActorJoinCompletion.Failed.class,
            "operationId", "kind");
        assertEquals(ActorRef.class,
            componentType(ZLinkActorJoinCompletion.Accepted.class, "actor"));

        assertEquals(ZLinkActorCreateCall.class, ZLinkActorManager.class
            .getMethod("create", String.class, String.class)
            .getReturnType());
        assertThrows(NoSuchMethodException.class, () -> ZLinkActorManager.class
            .getMethod("create", String.class, String.class, ZLinkMessage.class));
        assertEquals(CompletionStage.class, ZLinkActorManager.class
            .getMethod("find", String.class)
            .getReturnType());
        assertEquals(ZLinkActorGetOrCreateCall.class, ZLinkActorManager.class
            .getMethod("getOrCreate", String.class, String.class)
            .getReturnType());
        assertThrows(NoSuchMethodException.class, () -> ZLinkActorManager.class
            .getMethod(
                "getOrCreate",
                String.class,
                String.class,
                ZLinkMessage.class));
        for (Class<?> call : List.of(
            ZLinkActorCreateCall.class,
            ZLinkActorGetOrCreateCall.class)) {
            assertEquals(CompletionStage.class,
                call.getMethod("submit").getReturnType());
            assertEquals(CompletionStage.class,
                call.getMethod("yield").getReturnType());
        }
    }

    @Test
    void spotManagerPinsRelocatableSpotContract() throws Exception {
        assertEquals(ZLinkSpotCreateCall.class, ZLinkSpotManager.class
            .getMethod("create", String.class)
            .getReturnType());
        assertEquals(ZLinkSpotGetOrCreateCall.class, ZLinkSpotManager.class
            .getMethod("getOrCreate", String.class, String.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkSpotManager.class
            .getMethod("find", String.class)
            .getReturnType());
        assertEquals(CompletionStage.class, ZLinkSpotManager.class
            .getMethod("close", SpotRef.class)
            .getReturnType());
        assertEquals(ZLinkSpotCreateResult.class, ZLinkSpotCreateResult.class);
        assertEquals(ZLinkSpotInfo.class, ZLinkSpotInfo.class);
        assertNoPublicMethod(ZLinkSpotManager.class, "create", Class.class, Object.class);
        assertNoPublicMethod(ZLinkSpotManager.class, "getOrCreate", Class.class, RoutingId.class, Object.class);
    }

    @Test
    void routeClientPinsToNodeNaming() throws Exception {
        assertEquals(ZLinkSendCall.class, ZLinkRouteClient.class
            .getMethod("sendToNode", String.class, RoutingId.class, Object.class)
            .getReturnType());
        assertEquals(ZLinkRequestCall.class, ZLinkRouteClient.class
            .getMethod("requestToNode", String.class, RoutingId.class, Object.class)
            .getReturnType());
        assertEquals(ZLinkSendCall.class, ZLinkRouteClient.class
            .getMethod(
                "sendToChannel",
                String.class,
                Object.class)
            .getReturnType());
        assertEquals(ZLinkRequestCall.class, ZLinkRouteClient.class
            .getMethod(
                "requestToChannel",
                String.class,
                Object.class)
            .getReturnType());
        assertEquals(ZLinkSpotSendCall.class, ZLinkRouteClient.class
            .getMethod("sendToSpot", String.class, Object.class)
            .getReturnType());
        assertEquals(ZLinkSpotRequestCall.class, ZLinkRouteClient.class
            .getMethod("requestToSpot", String.class, Object.class)
            .getReturnType());
        assertNoPublicMethod(ZLinkRouteClient.class, "send", String.class, RoutingId.class, Object.class);
        assertNoPublicMethod(ZLinkRouteClient.class, "request", String.class, RoutingId.class, Object.class);
        assertNoPublicMethod(
            ZLinkRouteClient.class,
            "sendToSpot",
            String.class,
            RoutingId.class,
            RoutingId.class,
            Object.class);
        assertNoPublicMethod(
            ZLinkRouteClient.class,
            "requestToSpot",
            String.class,
            RoutingId.class,
            RoutingId.class,
            Object.class);
    }

    @Test
    void oldPublicContractSymbolsDoNotReenterJavaFrameworkConsumers() throws Exception {
        Path javaRoot = Path.of(System.getProperty("user.dir")).getParent();
        List<Path> scanRoots = List.of(
            javaRoot.resolve("zlink-framework-core"),
            javaRoot.resolve("zlink-framework-kotlin"),
            javaRoot.resolve("zlink-framework-locations-redis"),
            javaRoot.resolve("zlink-framework-spring-boot-starter"),
            javaRoot.resolve("e2e"),
            javaRoot.resolve("e2e-kotlin"),
            javaRoot.resolve("samples"));
        List<String> forbidden = List.of(
            "addPeerLocationStore",
            "addSpotLocationStore",
            "addActorLocationStore",
            "addRouteLocationStore",
            "addOwnerLeaseStore",
            "ZLinkRouteLocationResolver",
            "ZLinkActorRefResolver",
            "ZLinkSpotLocationResolver",
            "ZLinkActorLocationResolver",
            "resolveActorRef",
            "ResolveActorRef",
            "ZLinkRouteRequestCall",
            "ActorIdConflict",
            "ACTOR_ID_CONFLICT");

        List<String> offenders = new ArrayList<>();
        for (Path root : scanRoots) {
            if (!Files.exists(root)) {
                continue;
            }
            try (Stream<Path> files = Files.walk(root)) {
                files
                    .filter(Files::isRegularFile)
                    .filter(LocationContractTest::isGuardedSourceFile)
                    .filter(path -> !isAllowedGuardException(javaRoot, path))
                    .forEach(path -> collectForbiddenSymbols(javaRoot, path, forbidden, offenders));
            }
        }

        assertTrue(
            offenders.isEmpty(),
            "old public contract symbols reintroduced; allowed exceptions are build output, markdown docs, stream connector, and this guard test: "
                + offenders);
    }

    private static void assertMissing(String className) {
        assertThrows(ClassNotFoundException.class, () -> Class.forName(className));
    }

    private static void assertNoMethod(String name) {
        assertFalse(Arrays.stream(ZLinkFrameworkOptions.class.getMethods())
            .anyMatch(method -> method.getName().equals(name)));
    }

    private static void assertNoPublicMethod(Class<?> type, String name, Class<?>... parameterTypes) {
        assertThrows(NoSuchMethodException.class, () -> type.getMethod(name, parameterTypes));
    }

    private static void assertRecordComponents(Class<? extends Record> type, String... names) {
        assertEquals(
            List.of(names),
            Arrays.stream(type.getRecordComponents())
                .map(java.lang.reflect.RecordComponent::getName)
                .toList());
    }

    private static Class<?> componentType(Class<? extends Record> type, String name) {
        return Arrays.stream(type.getRecordComponents())
            .filter(component -> component.getName().equals(name))
            .findFirst()
            .orElseThrow()
            .getType();
    }

    private static <T extends Enum<T>> void assertEnumValues(Class<T> type, Map<String, Integer> expected)
        throws Exception {
        assertEquals(
            expected.keySet(),
            Arrays.stream(type.getEnumConstants())
                .map(Enum::name)
                .collect(Collectors.toCollection(java.util.LinkedHashSet::new)));
        Method value = type.getMethod("value");
        for (T constant : type.getEnumConstants()) {
            assertEquals(expected.get(constant.name()), value.invoke(constant), constant.name());
        }
    }

    private static boolean isGuardedSourceFile(Path path) {
        String name = path.getFileName().toString();
        return name.endsWith(".java") || name.endsWith(".kt") || name.endsWith(".kts");
    }

    private static boolean isAllowedGuardException(Path javaRoot, Path path) {
        Path relative = javaRoot.relativize(path);
        String normalized = relative.toString().replace('\\', '/');
        return normalized.contains("/build/")
            || normalized.contains("/.gradle/")
            || normalized.startsWith("zlink-stream-connector/")
            || normalized.equals("zlink-framework-core/src/test/java/systems/zlink/framework/LocationContractTest.java");
    }

    private static void collectForbiddenSymbols(
        Path javaRoot,
        Path path,
        List<String> forbidden,
        List<String> offenders) {
        try {
            List<String> lines = Files.readAllLines(path);
            for (int i = 0; i < lines.size(); i++) {
                String line = lines.get(i);
                for (String needle : forbidden) {
                    if (line.contains(needle)) {
                        offenders.add(javaRoot.relativize(path) + ":" + (i + 1) + " contains " + needle);
                    }
                }
            }
        } catch (IOException ex) {
            throw new AssertionError("failed to scan " + path, ex);
        }
    }
}
