package systems.zlink.framework.runtime.streams;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

final class ZLinkSessionPacketDispatcherRuntime<TSessionContext extends ZLinkSessionContext>
    implements ZLinkSessionPacketDispatcher<TSessionContext> {
    private final Map<String, Object> handlers;
    private final ZLinkMessageSerializer serializer;
    private final Executor handlerExecutor;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;

    ZLinkSessionPacketDispatcherRuntime(
        List<Class<?>> handlerTypes,
        ZLinkHandlerActivator handlerFactory,
        ZLinkMessageSerializer serializer,
        Executor handlerExecutor,
        List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers) {
        this.handlers = buildHandlerMap(handlerTypes, handlerFactory);
        this.serializer = java.util.Objects.requireNonNull(serializer, "serializer");
        this.handlerExecutor = java.util.Objects.requireNonNull(handlerExecutor, "handlerExecutor");
        this.suspendHandlerInvokers = java.util.List.copyOf(suspendHandlerInvokers);
    }

    @Override
    public CompletionStage<Boolean> tryHandle(
        TSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        Object handler =
            handlers.get(dispatch.packetName());
        if (handler == null) {
            return CompletableFuture.completedFuture(false);
        }
        if (handler instanceof ZLinkTypedSessionPacketHandler<?, ?> typedHandler) {
            return executeHandler(() -> invokeTypedHandler(typedHandler, context, dispatch, payload))
                .thenApply(ignored -> true);
        }
        Class<?> messageType = messageType(handler);
        if (messageType != null) {
            Object decoded = payload.decode(messageType);
            return executeHandler(() -> ZLinkHandlerMethodInvoker
                .invokeHandler(handler, "handle", new Object[] {context, dispatch, decoded}, suspendHandlerInvokers)
                .thenApply(ignored -> null))
                .thenApply(ignored -> true);
        }
        return executeHandler(() ->
            ZLinkHandlerMethodInvoker
                .invokeHandler(handler, "handle", new Object[] {context, dispatch, payload}, suspendHandlerInvokers)
            .thenApply(ignored -> null))
            .thenApply(ignored -> true);
    }

    @SuppressWarnings("unchecked")
    private CompletionStage<Void> invokeTypedHandler(
        ZLinkTypedSessionPacketHandler<?, ?> handler,
        TSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        ZLinkTypedSessionPacketHandler<TSessionContext, Object> typed =
            (ZLinkTypedSessionPacketHandler<TSessionContext, Object>) handler;
        Object decoded = payload.decode(typed.messageType());
        return ZLinkHandlerMethodInvoker
            .invokeHandler(typed, "handle", new Object[] {context, dispatch, decoded}, suspendHandlerInvokers)
            .thenApply(ignored -> null);
    }

    private <T> CompletionStage<T> executeHandler(
        java.util.function.Supplier<CompletionStage<T>> operation) {
        CompletableFuture<T> result = new CompletableFuture<>();
        try {
            handlerExecutor.execute(() -> {
                try {
                    operation.get().whenComplete((value, error) -> {
                        if (error != null) {
                            result.completeExceptionally(error);
                        } else {
                            result.complete(value);
                        }
                    });
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                }
            });
        } catch (RuntimeException ex) {
            result.completeExceptionally(ex);
        }
        return result;
    }

    private static <TSessionContext extends ZLinkSessionContext>
    Map<String, Object> buildHandlerMap(
        List<Class<?>> handlerTypes,
        ZLinkHandlerActivator handlerFactory) {
        Map<String, Object> map =
            new HashMap<>();
        for (Class<?> handlerType : handlerTypes) {
            Object handler = handlerFactory.create(handlerType);
            String packetName = packetName(handler);
            if (packetName == null || packetName.isBlank()
                || !packetName.equals(packetName.trim())) {
                throw new ZLinkConfigurationException(
                    "session packet handler must declare a non-empty packet name: "
                        + handlerType.getName());
            }
            if (map.putIfAbsent(packetName, handler) != null) {
                throw new ZLinkConfigurationException(
                    "duplicate session packet handler '" + packetName
                        + "' for stream node");
            }
        }
        return Map.copyOf(map);
    }

    private static String packetName(Object handler) {
        if (handler instanceof ZLinkTypedSessionPacketHandler<?, ?> typedHandler) {
            return ZLinkPacketNames.resolve(typedHandler.messageType());
        }
        try {
            Object value = handler.getClass().getMethod("packetName").invoke(handler);
            return value instanceof String text ? text : null;
        } catch (ReflectiveOperationException ex) {
            throw new ZLinkConfigurationException(
                "session packet handler must declare packetName(): "
                    + handler.getClass().getName());
        }
    }

    private static Class<?> messageType(Object handler) {
        try {
            Object value = handler.getClass().getMethod("messageType").invoke(handler);
            return value instanceof Class<?> klass ? klass : null;
        } catch (NoSuchMethodException ex) {
            return null;
        } catch (ReflectiveOperationException ex) {
            throw new ZLinkConfigurationException(
                "session packet handler messageType() failed: "
                    + handler.getClass().getName());
        }
    }
}
