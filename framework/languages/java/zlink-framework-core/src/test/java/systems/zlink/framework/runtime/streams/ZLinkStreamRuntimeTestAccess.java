package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.function.LongSupplier;

public final class ZLinkStreamRuntimeTestAccess {
    private ZLinkStreamRuntimeTestAccess() {}

    public static ZLinkStreamRuntime create(
            ZLinkBackendAdapterProvider backend,
            ZLinkFrameworkRegistration registration,
            ZLinkBackendContext context,
            LongSupplier clock,
            ScheduledExecutorService scheduler) {
        return new ZLinkStreamRuntime(
                backend,
                new ZLinkBackendAdapterOptions(registration.defaultRequestTimeout()),
                registration,
                Map.of(),
                Map.of(),
                new ZLinkJsonMessageSerializer(),
                null,
                ZLinkHandlerActivator.reflection(),
                null,
                null,
                context,
                false,
                (ignoredBackend, ignoredKey) ->
                        (ignoredSubmission, ignoredCleanup) ->
                                CompletableFuture.failedFuture(
                                        new IllegalStateException("test has no one-way admission")),
                clock,
                () -> scheduler);
    }
}
