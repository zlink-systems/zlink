package systems.zlink.framework;

import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
final class JavaTargetContractGapTest {
    @Test
    void handlersFactoriesAndLifecycleExposeCompletionStages() throws Exception {
        assertNamedMethodsReturnStage("systems.zlink.framework.channels.ZLinkRequestHandler", "handle");
        assertNamedMethodsReturnStage("systems.zlink.framework.channels.ZLinkSendHandler", "handle");
        assertNamedMethodsReturnStage("systems.zlink.framework.channels.ZLinkFanoutHandler", "handle");
        assertNamedMethodsReturnStage("systems.zlink.framework.actors.ZLinkActorFactory", "create");
    }

    @Test
    void oneWayCallsExposeOnlyAsyncAdmissionCompletion() throws Exception {
        assertClassAbsent("systems.zlink.framework.ZLinkSubmitStage");
        assertClassAbsent("systems.zlink.framework.ZLinkAwait");
        assertClassAbsent("systems.zlink.framework.channels.ZLinkYieldRequestCall");
        assertNamedMethodsReturnStage("systems.zlink.framework.channels.ZLinkSendCall", "submit");
        assertNamedMethodsReturnStage("systems.zlink.framework.channels.ZLinkFanoutPublishCall", "submit");
        assertNamedMethodsReturnStage("systems.zlink.framework.channels.ZLinkPublishCall", "submit");
        assertNamedMethodsReturnStage("systems.zlink.framework.actors.ZLinkActorSendCall", "submit");
        assertNamedMethodsReturnStage("systems.zlink.framework.actors.ZLinkBoundSessionSendCall", "submit");
        assertNamedMethodsReturnStage("systems.zlink.framework.streams.ZLinkSessionSendCall", "submit");
        assertNamedMethodsReturnStage("systems.zlink.framework.streams.ZLinkSessionReplyCall", "submit");
        assertPublicMethodReturnsStageOf(
            "systems.zlink.framework.streams.ZLinkSessionActor",
            "relay",
            Void.class);
        assertNoPublicMethodNamed("systems.zlink.framework.channels.ZLinkSendCall", "await");
        assertNoPublicMethodNamed("systems.zlink.framework.channels.ZLinkRequestCall", "await");
        assertNoPublicMethodNamed("systems.zlink.framework.actors.ZLinkActorSendCall", "await");
        assertNoPublicMethodNamed("systems.zlink.framework.actors.ZLinkActorRequestCall", "await");
        assertClassAbsent("systems.zlink.framework.actors.ZLinkActorJoinSpotCall");
    }

    @Test
    void javaContractDoesNotExportFrameworkCancellationToken() {
        assertClassAbsent("systems.zlink.framework.CancellationToken");
    }

    @Test
    void actorContextUsesOneMembershipValueAndOneJoinContract() throws Exception {
        Class<?> context = Class.forName("systems.zlink.framework.actors.ZLinkActorContext");
        assertNoPublicMethodNamed(context, "isJoined");
        assertNoPublicMethodNamed(context, "getSpot");
        Method spotId = context.getMethod("spotId");
        assertEquals(Optional.class, spotId.getReturnType());
        Class<?> call =
            Class.forName("systems.zlink.framework.actors.ZLinkActorJoinCall");
        assertEquals(void.class, call.getMethod("defer").getReturnType());
        assertNoPublicMethodNamed(call, "submit");
        assertClassAbsent("systems.zlink.framework.actors.ZLinkActorJoinResult");
        Class<?> completion =
            Class.forName("systems.zlink.framework.actors.ZLinkActorJoinCompletion");
        assertTrue(completion.isSealed(), "actor join completion must be sealed");
    }

    @Test
    void directActorMessagingAcceptsOnlyTheGlobalActorId() throws Exception {
        Class<?> client = Class.forName(
            "systems.zlink.framework.actors.ZLinkActorClient");
        assertNotNull(client.getMethod(
            "sendToActor", String.class, Object.class));
        assertNotNull(client.getMethod(
            "requestToActor", String.class, Object.class));
        Class<?> actorRef = Class.forName(
            "systems.zlink.framework.actors.ActorRef");
        assertFalse(Arrays.stream(client.getMethods()).anyMatch(method ->
            (method.getName().equals("sendToActor")
                || method.getName().equals("requestToActor"))
                && Arrays.asList(method.getParameterTypes()).contains(
                    actorRef)));
    }

    @Test
    void streamActorDispatchUsesTheGlobalAuthorityContract() throws Exception {
        Class<?> builder = Class.forName(
            "systems.zlink.framework.configuration.ZLinkStreamNodeBuilder");
        assertNotNull(builder.getMethod("enableActorDispatch"));
        assertFalse(Arrays.stream(builder.getMethods()).anyMatch(method ->
            method.getName().equals("enableActorDispatch")
                && method.getParameterCount() != 0));
    }

    @Test
    void streamNodeExposesTheBoundedMessageSizeContract() throws Exception {
        Class<?> builder = Class.forName(
            "systems.zlink.framework.configuration.ZLinkStreamNodeBuilder");
        Class<?> socketConfig = Class.forName(
            "systems.zlink.framework.configuration.ZLinkStreamSocketConfig");
        assertEquals(socketConfig, builder.getMethod("configureSocket").getReturnType());
        assertEquals(long.class, socketConfig.getMethod("maxMessageSize").getReturnType());
        assertEquals(void.class, socketConfig.getMethod("setMaxMessageSize", long.class)
            .getReturnType());
    }

    @Test
    void spotAddressIsHiddenBehindOpaqueHandle() throws Exception {
        assertNotNull(Class.forName("systems.zlink.framework.spots.SpotHandle"));
        assertNotNull(Class.forName("systems.zlink.framework.spots.SpotHandleResolver"));
        assertNotNull(Class.forName("systems.zlink.framework.spots.ActorSpotHandleResolver"));
        assertClassAbsent("systems.zlink.framework.spots.SpotRemoteRef");
        assertClassAbsent("systems.zlink.framework.spots.SpotRemoteRefResolver");
    }

    @Test
    void dispatchStrategyAndTypedPacketOverridesAreNotPublic() throws Exception {
        assertClassAbsent("systems.zlink.framework.configuration.ZLinkDispatchMode");
        assertNoPublicMethodNamed("systems.zlink.framework.channels.ZLinkSendCall", "packetName");
        assertNoPublicMethodNamed("systems.zlink.framework.channels.ZLinkRequestCall", "packetName");
    }

    @Test
    void spotManagerExposesRequestCreationAndContextCloseContracts() throws Exception {
        // spots.ko.md fixes ZLinkSpotManager on the fluent create calls: the creation request
        // rides ZLinkSpotCreateCall.request(...), not a second create(...) parameter.
        Class<?> manager = Class.forName("systems.zlink.framework.spots.ZLinkSpotManager");
        Class<?> createCall = Class.forName("systems.zlink.framework.spots.ZLinkSpotCreateCall");
        Class<?> getOrCreateCall =
            Class.forName("systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall");
        assertEquals(createCall, manager.getMethod("create", String.class).getReturnType());
        assertEquals(
            getOrCreateCall,
            manager.getMethod("getOrCreate", String.class, String.class).getReturnType());
        assertFalse(Arrays.stream(manager.getMethods()).anyMatch(method ->
            method.getName().equals("create") && method.getParameterCount() != 1));
        for (Class<?> call : new Class<?>[] {createCall, getOrCreateCall}) {
            assertEquals(call, call.getMethod("request", Object.class).getReturnType());
            assertEquals(
                call,
                call.getMethod("request", Class.forName("systems.zlink.framework.messaging.ZLinkMessage"))
                    .getReturnType());
        }
        Class<?> context = Class.forName("systems.zlink.framework.spots.ZLinkSpotContext");
        assertEquals(CompletionStage.class, context.getMethod("close").getReturnType());
    }

    @Test
    void routeMeshAndManualConnectionsUseTheTargetRuntimeContracts() throws Exception {
        Class<?> options = Class.forName("systems.zlink.framework.channels.ZLinkChannelRuntimeOptions");
        assertNotNull(options.getMethod("routeMeshChannel", String.class));
        assertNotNull(Class.forName("systems.zlink.framework.channels.ZLinkRouteMeshChannelRuntimeOptions"));
        Class<?> connections = Class.forName(
            "systems.zlink.framework.configuration.ZLinkEndpointConnections");
        assertNotNull(connections.getMethod("connect", String.class));
        assertNotNull(connections.getMethod("disconnect", String.class));
        assertNotNull(connections.getMethod("listConnections"));
        // channel-messaging.ko.md fixes ClientServerChannelBuilder on client()/server()
        // sub-builders; the flat clientConnections()/enableClient(...) surface is gone.
        Class<?> clientServer =
            Class.forName("systems.zlink.framework.configuration.ClientServerChannelBuilder");
        Class<?> clientBuilder = Class.forName(
            "systems.zlink.framework.configuration.ZLinkClientServerChannelClientBuilder");
        assertEquals(clientBuilder, clientServer.getMethod("client").getReturnType());
        assertEquals(
            Class.forName("systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder"),
            clientServer.getMethod("server").getReturnType());
        assertEquals(clientBuilder, clientBuilder.getMethod("connect", String.class).getReturnType());
        assertNoPublicMethodNamed(clientServer, "clientConnections");
        assertNoPublicMethodNamed(clientServer, "enableClient");
        assertNotNull(Class.forName("systems.zlink.framework.configuration.FanoutChannelBuilder")
            .getMethod("subscriberConnections"));
        assertEquals(
            Class.forName("systems.zlink.framework.configuration.FanoutChannelBuilder"),
            Class.forName("systems.zlink.framework.configuration.FanoutChannelBuilder")
                .getMethod("setRoutingIdPrefix", String.class)
                .getReturnType());
        assertClassAbsent(
            "systems.zlink.framework.configuration.RouteMeshChannelBuilder");
        assertClassAbsent(
            "systems.zlink.framework.configuration.ZLinkSpotNodeBuilder");
    }

    @Test
    void typedSessionHandlerDoesNotInheritRawApplicationHandler() throws Exception {
        Class<?> typed = Class.forName("systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler");
        assertClassAbsent("systems.zlink.framework.streams.ZLinkSessionPacketHandler");
        assertNamedMethodsReturnStage(typed.getName(), "handle");

        Class<?> streamError = Class.forName("systems.zlink.framework.streams.ZLinkStreamError");
        assertEquals(
            List.of("error", "message"),
            Arrays.stream(streamError.getRecordComponents())
                .map(component -> component.getName())
                .toList());
        assertClassAbsent("systems.zlink.framework.streams.ZLinkStreamDiagnostic");
    }

    @Test
    void observabilityOperationsContractIsExported() throws Exception {
        assertNotNull(Class.forName("systems.zlink.framework.monitoring.ZLinkFlowOrigin"));
        assertClassAbsent("systems.zlink.framework.configuration.ZLinkFlowOrigin");
        assertNotNull(Class.forName(
            "systems.zlink.framework.locations.ZLinkActivationConcurrency"));
        assertClassAbsent(
            "systems.zlink.framework.monitoring.ZLinkActivationConcurrency");
        assertClassAbsent(
            "systems.zlink.framework.monitoring.ZLinkRuntimeEventDispatcher");
        assertClassAbsent("systems.zlink.framework.monitoring.ZLinkDrainControl");
        assertClassAbsent("systems.zlink.framework.monitoring.ZLinkDrainResult");
        assertClassAbsent("systems.zlink.framework.monitoring.ZLinkMeshDrainResult");
        assertClassAbsent("systems.zlink.framework.monitoring.ZLinkMeshDrainSnapshot");
        assertNoPublicMethodNamed(
            "systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot", "drain");
        assertClassAbsent("systems.zlink.framework.runtime.internal.locations.ZLinkPeerLocation");
        assertNotNull(Class.forName(
            "systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor"));
    }

    @Test
    void backendAndHandlerRuntimeTypesAreNotApplicationPublicSurface() throws Exception {
        assertFalse(
            Thread.currentThread().getContextClassLoader()
                .getResources("systems/zlink/contracts/service/spot")
                .hasMoreElements(),
            "legacy service Spot package must not be exported");
        assertFalse(
            Thread.currentThread().getContextClassLoader()
                .getResources("systems/zlink/framework/runtime/backend")
                .hasMoreElements(),
            "backend SPI package must not be exported");
        assertFalse(
            Thread.currentThread().getContextClassLoader()
                .getResources("systems/zlink/framework/runtime/service")
                .hasMoreElements(),
            "service runtime implementation package must not be exported");
        assertClassAbsent("systems.zlink.framework.runtime.backend.ZLinkBackendAdapterFactory");
        assertClassAbsent("systems.zlink.framework.runtime.backend.ZLinkBackendSpotNode");
        assertClassAbsent("systems.zlink.framework.runtime.handlers.ZLinkHandlerFactory");
        assertClassAbsent("systems.zlink.framework.runtime.handlers.ZLinkSuspendHandlerInvoker");

        assertInternalRuntimeSpi(
            "systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider");
        assertInternalRuntimeSpi(
            "systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode");
        assertInternalRuntimeSpi(
            "systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator");
        assertInternalRuntimeSpi(
            "systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter");

        assertNotPublic(
            "systems.zlink.framework.runtime.internal.service.ZLinkCanonicalRelocationControlCodec");
        assertNotPublic(
            "systems.zlink.framework.runtime.internal.service.ZLinkInMemoryLocationAuthority");
        for (String type : new String[] {
            "ActorTransferControl",
            "ActorTransferPhase",
            "FrameworkReadyBatch",
            "FrameworkReceiveBatch",
            "MeshRecordPayload",
            "MeshMonitorEventMask",
            "MeshNodeOptions",
            "MessageBuilderStage",
            "ReplySubmitOperation",
            "RequestCallbackSubmitOperation",
            "RequestSubmitOperation",
            "SpotKind",
            "StreamSessionBinding",
            "StreamSessionService",
            "StreamSessionState",
            "StreamSessionStatus",
            "TimeoutSubmitOperation"
        }) {
            assertNotPublic(
                "systems.zlink.framework.runtime.internal.binding.spot." + type);
        }
        for (String type : new String[] {
            "ZLinkClassicFanoutLiveness",
            "ZLinkServiceFrozenRecordCodec",
            "ZLinkServiceLivenessRegistry",
            "ZLinkServiceM6AWireCodec",
            "ZLinkServiceM6BWireCodec",
            "ZLinkServiceMailbox",
            "ZLinkServiceNodeDescriptor",
            "ZLinkServiceOperationRegistry",
            "ZLinkServiceRelocationWireCodec",
            "ZLinkServiceTopologyRegistry",
            "ZLinkServiceWireCodec",
            "ZLinkServiceWireFrame"
        }) {
            assertInternalRuntimeSpi(
                "systems.zlink.framework.runtime.internal.service." + type);
        }
    }

    @Test
    void completionStageOperationsDoNotUseAsyncSuffix() throws Exception {
        Class<?> store = Class.forName("systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository");
        for (Method method : store.getMethods()) {
            if (CompletionStage.class.isAssignableFrom(method.getReturnType())) {
                assertFalse(method.getName().endsWith("Async"), method.toString());
            }
        }
    }

    @Test
    void locationContractKeepsOnlyProviderAndOperationalFacets() throws Exception {
        assertClassAbsent("systems.zlink.framework.locations.ZLinkLocationKey");
        assertNotNull(Class.forName(
            "systems.zlink.framework.locations.ZLinkLocationReadiness"));
        assertClassAbsent("systems.zlink.framework.locations.ZLinkLocationWatchStore");
        assertClassAbsent("systems.zlink.framework.locations.ZLinkOwnerLease");
        assertClassAbsent("systems.zlink.framework.locations.ZLinkOwnerLeaseSnapshot");
        assertClassAbsent("systems.zlink.framework.locations.ZLinkOwnerLeaseRenewal");
        assertNotNull(Class.forName("systems.zlink.framework.locations.ZLinkLocationRuntimeQuery"));
        assertNotNull(Class.forName("systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository"));
    }

    @Test
    void locationOptionsExposeObjectRoutingAndRelocationBudgets() throws Exception {
        Class<?> options = Class.forName(
            "systems.zlink.framework.locations.ZLinkLocationOptions");
        for (String getter : new String[] {
            "ownerLeaseRenewInterval",
            "ownerLeaseFencingMargin",
            "routeCacheMaxAge",
            "messageFollowDuration",
            "maxActiveOutboundRelocations",
            "maxActiveInboundRelocations",
            "maxConcurrentRelocationCaptures",
            "maxConcurrentRelocationRestores",
            "maxRelocationPayloadInFlightBytes"
        }) {
            assertNotNull(options.getMethod(getter), getter);
        }
        assertNoPublicMethodNamed(options, "heartbeatInterval");
        assertNoPublicMethodNamed(options, "routingIdFencingMargin");
    }

    @Test
    void javaCoreDoesNotReimplementKotlinSuspendInvocation() throws Exception {
        Class<?> invoker = Class.forName("systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker");
        assertFalse(Arrays.stream(invoker.getDeclaredMethods()).anyMatch(method ->
            method.getName().equals("invokeKotlinSuspend")),
            "Kotlin module provider must be the only coroutine invocation owner");
    }

    private static void assertNamedMethodsReturnStage(String className, String methodName) throws Exception {
        Class<?> type = Class.forName(className);
        Method[] methods = Arrays.stream(type.getMethods())
            .filter(method -> method.getName().equals(methodName))
            .toArray(Method[]::new);
        assertTrue(methods.length > 0, className + "." + methodName);
        for (Method method : methods) {
            assertTrue(CompletionStage.class.isAssignableFrom(method.getReturnType()), method.toString());
        }
    }

    private static void assertPublicMethodReturnsStageOf(
        String className,
        String methodName,
        Class<?> resultType) throws Exception {
        Class<?> type = Class.forName(className);
        Method[] methods = Arrays.stream(type.getMethods())
            .filter(candidate -> candidate.getName().equals(methodName))
            .toArray(Method[]::new);
        assertTrue(methods.length > 0, className + "." + methodName);
        for (Method method : methods) {
            assertTrue(CompletionStage.class.isAssignableFrom(method.getReturnType()), method.toString());
            var generic = method.getGenericReturnType().getTypeName();
            assertTrue(generic.contains(resultType.getName()), generic);
        }
    }

    private static void assertNoPublicMethodNamed(String className, String methodName) throws Exception {
        assertNoPublicMethodNamed(Class.forName(className), methodName);
    }

    private static void assertNoPublicMethodNamed(Class<?> type, String methodName) {
        assertFalse(Arrays.stream(type.getMethods()).anyMatch(method -> method.getName().equals(methodName)),
            type.getName() + "." + methodName);
    }

    private static void assertNotPublic(String className) throws Exception {
        Class<?> type = Class.forName(className);
        assertFalse(Modifier.isPublic(type.getModifiers()), className + " must not be public");
    }

    private static void assertInternalRuntimeSpi(String className) throws Exception {
        Class<?> type = Class.forName(className);
        assertTrue(type.getPackageName().startsWith("systems.zlink.framework.runtime.internal."));
        assertFalse(type.getPackageName().startsWith("systems.zlink.framework.configuration"));
        assertFalse(type.getPackageName().startsWith("systems.zlink.framework.channels"));
        assertFalse(type.getPackageName().startsWith("systems.zlink.framework.spots"));
        assertFalse(type.getPackageName().startsWith("systems.zlink.framework.streams"));
    }

    private static void assertClassAbsent(String className) {
        try {
            Class.forName(className);
            fail(className + " must not be exported");
        } catch (ClassNotFoundException expected) {
            // Target contract reached.
        }
    }

}
