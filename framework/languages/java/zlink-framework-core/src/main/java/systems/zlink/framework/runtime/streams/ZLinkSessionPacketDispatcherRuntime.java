package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;

final class ZLinkSessionPacketDispatcherRuntime<TSessionContext extends ZLinkSessionContext>
        implements ZLinkSessionPacketDispatcher<TSessionContext> {
    private final Map<String, Handler> handlers;
    private final Executor handlerExecutor;

    ZLinkSessionPacketDispatcherRuntime(
            List<Class<?>> handlerTypes,
            ZLinkHandlerActivator handlerFactory,
            ZLinkMessageSerializer serializer,
            Executor handlerExecutor,
            List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers) {
        Objects.requireNonNull(serializer, "serializer");
        this.handlers =
                buildHandlerMap(handlerTypes, handlerFactory, serializer, suspendHandlerInvokers);
        this.handlerExecutor = Objects.requireNonNull(handlerExecutor, "handlerExecutor");
    }

    @Override
    public CompletionStage<Boolean> tryHandle(
            TSessionContext context, ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        Handler handler = handlers.get(dispatch.packetName());
        if (handler == null) {
            return CompletableFuture.completedFuture(false);
        }
        return executeHandler(
                        () -> {
                            Object decoded =
                                    handler.messageType() == null
                                            ? payload
                                            : payload.decode(handler.messageType());
                            return handler.invocation()
                                    .invoke(handler.instance(), context, dispatch, decoded)
                                    .thenApply(ignored -> null);
                        })
                .thenApply(ignored -> true);
    }

    private <T> CompletionStage<T> executeHandler(Supplier<CompletionStage<T>> operation) {
        CompletableFuture<T> result = new CompletableFuture<>();
        var applicationJob = ZLinkApplicationJobContext.transferToQueuedJob();
        try {
            handlerExecutor.execute(
                    () -> {
                        try (var ignored = ZLinkApplicationJobContext.enterQueued(applicationJob)) {
                            ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                            operation
                                    .get()
                                    .whenComplete(
                                            (value, error) -> {
                                                if (error != null) {
                                                    result.completeExceptionally(error);
                                                } else {
                                                    result.complete(value);
                                                }
                                            });
                        } catch (RuntimeException ex) {
                            result.completeExceptionally(ex);
                        } finally {
                            if (applicationJob != null) {
                                applicationJob.close();
                            }
                        }
                    });
        } catch (RuntimeException ex) {
            if (applicationJob != null) {
                applicationJob.close();
            }
            result.completeExceptionally(ex);
        }
        return result;
    }

    private static <TSessionContext extends ZLinkSessionContext>
            Map<String, Handler> buildHandlerMap(
                    List<Class<?>> handlerTypes,
                    ZLinkHandlerActivator handlerFactory,
                    ZLinkMessageSerializer serializer,
                    List<ZLinkSuspendInvocationAdapter> suspendInvokers) {
        Map<String, Handler> map = new HashMap<>();
        for (Class<?> handlerType : handlerTypes) {
            Object handler = handlerFactory.create(handlerType);
            Class<?> messageType =
                    handler instanceof ZLinkTypedSessionPacketHandler<?, ?> typed
                            ? typed.messageType()
                            : messageType(handler);
            if (messageType != null) serializer.prepare(messageType);
            var method =
                    ZLinkHandlerMethodInvoker.requireHandlerMethodForTypes(
                            handlerType,
                            ZLinkHandlerMethodInvoker.HANDLER_METHOD_NAME,
                            null,
                            ZLinkSessionDispatchContext.class,
                            messageType == null ? ZLinkMessage.class : messageType);
            var invocation = ZLinkHandlerMethodInvoker.prepare(method, suspendInvokers, 0, 1, 2);
            String packetName =
                    handler instanceof ZLinkTypedSessionPacketHandler<?, ?>
                            ? ZLinkPacketNames.resolve(messageType)
                            : packetName(handler);
            if (packetName == null
                    || packetName.isBlank()
                    || !packetName.equals(packetName.trim())) {
                throw new ZLinkConfigurationException(
                        "session packet handler must declare a non-empty packet name: "
                                + handlerType.getName());
            }
            if (map.putIfAbsent(packetName, new Handler(handler, messageType, invocation))
                    != null) {
                throw new ZLinkConfigurationException(
                        "duplicate session packet handler '" + packetName + "' for stream node");
            }
        }
        return Map.copyOf(map);
    }

    private static String packetName(Object handler) {
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
                    "session packet handler messageType() failed: " + handler.getClass().getName());
        }
    }

    private record Handler(
            Object instance,
            Class<?> messageType,
            ZLinkHandlerMethodInvoker.PreparedMethod invocation) {}
}
