package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.annotation.Repeatable;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkHandlerGroups;
import systems.zlink.framework.handlers.ZLinkPublish;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.handlers.ZLinkSend;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.handlers.ZLinkSpotTimer;
import systems.zlink.framework.handlers.ZLinkStreamPacket;
import systems.zlink.framework.handlers.ZLinkStreamRaw;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkSessionReplyCall;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

final class HandlerContractTest {
    @Test
    void packetAnnotationsUseDocumentedAttributeNames() {
        assertAnnotationMethods(ZLinkHandlerGroup.class, "value");
        assertAnnotationMethods(ZLinkHandlerGroups.class, "value");
        assertAnnotationMethods(ZLinkRequest.class, "packetName");
        assertAnnotationMethods(ZLinkSend.class, "packetName");
        assertAnnotationMethods(ZLinkPublish.class, "packetName");
        assertAnnotationMethods(ZLinkSpotRequest.class, "packetName");
        assertAnnotationMethods(ZLinkSpotActorRequest.class, "packetName");
        assertAnnotationMethods(ZLinkSpotActorSend.class, "packetName");
        assertAnnotationMethods(ZLinkSpotSubscription.class, "spotNodeName", "topic");
        assertAnnotationMethods(ZLinkSpotTimer.class, "name", "periodMillis");
        assertAnnotationMethods(ZLinkStreamPacket.class);
        assertAnnotationMethods(ZLinkStreamRaw.class);
    }

    @Test
    void handlerGroupAnnotationIsRepeatable() {
        Repeatable repeatable = ZLinkHandlerGroup.class.getAnnotation(Repeatable.class);

        assertSame(ZLinkHandlerGroups.class, repeatable.value());
    }

    @Test
    void timerOptionsAreImmutableExactInterfaceRecord() {
        assertTrue(ZLinkTimerOptions.class.isRecord());
        assertEquals(
            List.of("overrunPolicy", "maxCatchUpTicks", "stopOnUnhandledException"),
            Arrays.stream(ZLinkTimerOptions.class.getRecordComponents())
                .map(component -> component.getName())
                .toList());
        assertFalse(hasMethod(ZLinkTimerOptions.class, "setOverrunPolicy"));
        assertFalse(hasMethod(ZLinkTimerOptions.class, "setMaxCatchUpTicks"));
        assertFalse(hasMethod(ZLinkTimerOptions.class, "setStopOnUnhandledException"));
    }

    @Test
    void handlerFilterUsesFilterSpecificContextAndTypedNext()
        throws NoSuchMethodException {
        Method method = ZLinkHandlerFilter.class.getMethod(
            "invoke",
            ZLinkHandlerFilterContext.class,
            ZLinkHandlerFilterNext.class);

        assertEquals(1, method.getTypeParameters().length);
        assertTrue(ZLinkMessageContext.class.isAssignableFrom(
            ZLinkHandlerFilterContext.class));
        ZLinkHandlerFilterContext.class.getMethod("dispatchKind");
        assertEquals(
            List.of(
                ZLinkHandlerDispatchKind.NODE_DIRECT_SEND,
                ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST,
                ZLinkHandlerDispatchKind.CHANNEL_SEND,
                ZLinkHandlerDispatchKind.CHANNEL_REQUEST,
                ZLinkHandlerDispatchKind.CLASSIC_FANOUT),
            List.of(ZLinkHandlerDispatchKind.values()));
    }

    @Test
    void objectAndMessageContextsMatchTheExactInterface() throws Exception {
        assertFalse(hasMethod(ZLinkActor.class, "actorId"));
        assertSame(
            ZLinkActorContext.class,
            ZLinkActor.class.getMethod("context").getReturnType());
        ZLinkActorFactory.class.getMethod("create", ZLinkActorContext.class);

        ZLinkMessageContext.class.getMethod("meshName");
        ZLinkMessageContext.class.getMethod("channelName");
        ZLinkMessageContext.class.getMethod("packetName");
        ZLinkMessageContext.class.getMethod("contentType");
        ZLinkMessageContext.class.getMethod("metadata");
        ZLinkMessageContext.class.getMethod("correlationId");

        assertClassIsAbsent("systems.zlink.framework.ZLinkHandlerContext");
        assertClassIsAbsent("systems.zlink.framework.ZLinkInvocationContext");
        assertClassIsAbsent("systems.zlink.framework.ZLinkHandlerInvocation");
        assertClassIsAbsent("systems.zlink.framework.ZLinkNext");
        assertClassIsAbsent("systems.zlink.framework.MeshNodePublisher");
        assertClassIsAbsent("systems.zlink.framework.channels.ZLinkRequestContext");
        assertClassIsAbsent("systems.zlink.framework.channels.ZLinkSendContext");
        assertClassIsAbsent("systems.zlink.framework.channels.ZLinkPublishContext");
        assertClassIsAbsent("systems.zlink.framework.channels.ZLinkRouteRequestContext");
        assertClassIsAbsent("systems.zlink.framework.channels.ZLinkRouteSendContext");
        assertClassIsAbsent("systems.zlink.framework.spots.ZLinkSpotActorRequestContext");
        assertClassIsAbsent("systems.zlink.framework.spots.ZLinkSpotActorSendContext");
    }

    @Test
    void metadataContractsMatchTheExactInterface() throws NoSuchMethodException {
        assertEquals(
            java.util.Map.class,
            ZLinkMessageContext.class.getMethod("metadata").getReturnType());
        assertEquals(
            ZLinkMetadataPolicyBuilder.class,
            ZLinkMetadataPolicyBuilder.class
                .getMethod("allowSessionToActor", String.class)
                .getReturnType());
        assertEquals(
            ZLinkMetadataPolicyBuilder.class,
            ZLinkMetadataPolicyBuilder.class
                .getMethod("allowActorToSession", String.class)
                .getReturnType());
        assertFalse(hasMethod(ZLinkMetadataPolicyBuilder.class, "addForwardedMetadataKey"));
        assertFalse(hasMethod(ZLinkSessionReplyCall.class, "metadata"));
    }

    @Test
    void requestCallExposesYieldWithoutBlockingAwait() throws NoSuchMethodException {
        ZLinkRequestCall.class.getMethod("yield", Class.class);
        assertFalse(hasMethod(ZLinkRequestCall.class, "await"));
    }

    @Test
    void channelCallsExposeApplicationMetadataBuilders() {
        assertTrue(hasMethod(ZLinkSendCall.class, "metadata"));
        assertTrue(hasMethod(ZLinkRequestCall.class, "metadata"));
        assertTrue(hasMethod(ZLinkPublishCall.class, "metadata"));
    }

    @Test
    void spotHandlerRegistryMatchesDotnetRegistrationSurface() throws NoSuchMethodException {
        ZLinkSpotHandlerRegistry.class.getMethod("addHandler", Class.class);
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addPacket"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addSubscribe"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addActorPacket"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addActorSend"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addActorRequest"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addActor" + "Join"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addPostActor" + "Joined"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addActor" + "Left"));
        assertFalse(hasMethod(ZLinkSpotHandlerRegistry.class, "addActor" + "Disconnected"));

        ZLinkSpotPacketHandler.class.getMethod("handle", Object.class, Object.class);
        ZLinkSpotRequestHandler.class.getMethod("handle", Object.class, Object.class);
        ZLinkSpotSubscriptionHandler.class.getMethod("handle", Object.class, Object.class);
        ZLinkEntrySpotActorSendHandler.class.getMethod(
            "handle",
            ZLinkEntrySpot.class,
            systems.zlink.framework.actors.ZLinkActor.class,
            systems.zlink.framework.ZLinkMessageContext.class,
            Object.class);
        ZLinkEntrySpotActorRequestHandler.class.getMethod(
            "handle",
            ZLinkEntrySpot.class,
            systems.zlink.framework.actors.ZLinkActor.class,
            systems.zlink.framework.ZLinkMessageContext.class,
            Object.class);
        ZLinkSpotActorSendHandler.class.getMethod(
            "handle",
            ZLinkSpot.class,
            systems.zlink.framework.actors.ZLinkActor.class,
            systems.zlink.framework.ZLinkMessageContext.class,
            Object.class);
        ZLinkSpotActorRequestHandler.class.getMethod(
            "handle",
            ZLinkSpot.class,
            systems.zlink.framework.actors.ZLinkActor.class,
            systems.zlink.framework.ZLinkMessageContext.class,
            Object.class);
    }

    @Test
    void spotLifecycleCallbacksAreMemberContracts() throws NoSuchMethodException {
        ZLinkSpot.class.getMethod("onCreate", ZLinkMessage.class);
        ZLinkSpot.class.getMethod(
            "onActorJoin",
            String.class,
            ZLinkMessage.class);
        ZLinkSpot.class.getMethod("onJoinedActor", ZLinkActor.class);
        ZLinkSpot.class.getMethod("onLeaveActor", ZLinkActor.class);
        ZLinkSpot.class.getMethod("onDisconnectActor", ZLinkActor.class);
        assertThrows(NoSuchMethodException.class, () -> ZLinkEntrySpot.class.getMethod(
            "onActorJoin",
            String.class,
            ZLinkMessage.class));
        ZLinkEntrySpot.class.getMethod(
            "onCreateActor",
            ZLinkActor.class,
            ZLinkMessage.class);
        assertThrows(NoSuchMethodException.class, () -> ZLinkEntrySpot.class.getMethod(
            "onActorRelocated",
            ZLinkActor.class));
        assertEquals(CompletionStage.class,
            ZLinkSpot.class.getMethod("onJoinedActor", ZLinkActor.class).getReturnType());
        assertEquals(CompletionStage.class,
            ZLinkSpot.class.getMethod("onLeaveActor", ZLinkActor.class).getReturnType());
        assertEquals(CompletionStage.class,
            ZLinkEntrySpot.class.getMethod("onJoinedActor", ZLinkActor.class).getReturnType());
        assertEquals(CompletionStage.class,
            ZLinkEntrySpot.class.getMethod("onLeaveActor", ZLinkActor.class).getReturnType());
        assertTrue(ZLinkSpotActorJoinResult.accept().accepted());
    }

    @Test
    void sessionDispatchContractsUseFrameworkMessages() throws NoSuchMethodException {
        ZLinkSession.class.getMethod("onDispatch", ZLinkSessionDispatchContext.class, ZLinkMessage.class);
        assertClassMissing("systems.zlink.framework.streams.ZLinkSessionPacketHandler");
        ZLinkTypedSessionPacketHandler.class.getMethod(
            "handle",
            ZLinkSessionContext.class,
            ZLinkSessionDispatchContext.class,
            Object.class);
        assertEquals(CompletionStage.class,
            ZLinkTypedSessionPacketHandler.class.getMethod(
                "handle",
                ZLinkSessionContext.class,
                ZLinkSessionDispatchContext.class,
                Object.class).getReturnType());
        ZLinkSessionPacketDispatcher.class.getMethod(
            "tryHandle",
            ZLinkSessionContext.class,
            ZLinkSessionDispatchContext.class,
            ZLinkMessage.class);
        ZLinkSessionActor.class.getMethod("relay", ZLinkMessage.class);
    }

    @Test
    void actorJoinContractsSupportDtoAndNoReplyJoins() throws NoSuchMethodException {
        ZLinkActorContext.class.getMethod("joinSpot", String.class);
        ZLinkActorContext.class.getMethod("joinSpot", String.class, Object.class);
        ZLinkActorContext.class.getMethod("joinEntrySpot");
        ZLinkActorContext.class.getMethod("joinEntrySpot", Object.class);
        ZLinkActorJoinCall.class.getMethod("timeout", java.time.Duration.class);
        ZLinkActorJoinCall.class.getMethod("defer");
        assertFalse(hasMethod(ZLinkActorJoinCall.class, "submit"));
        assertFalse(hasMethod(ZLinkActorJoinCall.class, "yield"));
        assertFalse(hasMethod(ZLinkActorContext.class, "isJoined"));
        assertFalse(hasMethod(ZLinkActorContext.class, "getSpot"));
        assertFalse(hasMethod(ZLinkActorJoinCall.class, "await"));
    }

    @Test
    void executionTerminatorsAndUserSpotOptionsMatchExactContract() throws Exception {
        assertTrue(Modifier.isAbstract(
            ZLinkRequestCall.class.getMethod("yield", Class.class).getModifiers()));
        assertTrue(Modifier.isAbstract(
            systems.zlink.framework.actors.ZLinkActorRequestCall.class
                .getMethod("yield", Class.class)
                .getModifiers()));
        assertTrue(Modifier.isAbstract(
            systems.zlink.framework.spots.ZLinkWorkerCall.class
                .getMethod("yield")
                .getModifiers()));
        assertFalse(hasMethod(ZLinkActorJoinCall.class, "yield"));

        assertEquals(
            1,
            systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
                .PER_ACTOR.value());
    }

    @Test
    void oldSpotActorLifecyclePublicContractsAreRemoved() {
        assertClassMissing("systems.zlink.framework.handlers.ZLinkSpotActor" + "Join");
        assertClassMissing("systems.zlink.framework.handlers.ZLinkSpotPostActor" + "Joined");
        assertClassMissing("systems.zlink.framework.handlers.ZLinkSpotActor" + "Left");
        assertClassMissing("systems.zlink.framework.spots.ZLinkSpotActor" + "JoinHandler");
        assertClassMissing("systems.zlink.framework.spots.ZLinkSpotPostActor" + "JoinedHandler");
        assertClassMissing("systems.zlink.framework.spots.ZLinkSpotActor" + "LeftHandler");
        assertClassMissing("systems.zlink.framework.handlers.ZLinkSpotActor" + "Disconnected");
        assertClassMissing("systems.zlink.framework.spots.ZLinkSpotActor" + "DisconnectedHandler");
        assertClassMissing("systems.zlink.framework.spots.ZLinkEntrySpotActor" + "DisconnectedHandler");
        assertClassMissing("systems.zlink.framework.spots.ZLinkSpotActorChange" + "Result");
        assertClassMissing("systems.zlink.framework.spots.ZLinkSpotActorChange" + "Kind");
    }

    private static void assertAnnotationMethods(Class<?> annotationType, String... expectedNames) {
        String[] actualNames = Arrays.stream(annotationType.getDeclaredMethods())
            .map(Method::getName)
            .sorted()
            .toArray(String[]::new);
        Arrays.sort(expectedNames);

        assertTrue(
            Arrays.equals(expectedNames, actualNames),
            () -> annotationType.getSimpleName()
                + " methods were "
                + Arrays.toString(actualNames));
    }

    private static boolean hasMethod(Class<?> type, String name) {
        return Arrays.stream(type.getMethods())
            .anyMatch(method -> method.getName().equals(name));
    }

    private static void assertClassIsAbsent(String className) {
        assertThrows(ClassNotFoundException.class, () -> Class.forName(className));
    }

    private static void assertClassMissing(String className) {
        try {
            Class.forName(className);
        } catch (ClassNotFoundException expected) {
            return;
        }
        throw new AssertionError("class should be removed: " + className);
    }
}
