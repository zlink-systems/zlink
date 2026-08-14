package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

final class ZLinkSessionRelocationSealTimeoutWiringTest {
    @Test
    void configuredRootLocationValueReachesTheActualSessionSealOwner()
        throws Exception {
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        Object locations = options.configureLocations();
        locations.getClass()
            .getMethod("setSessionRelocationSealTimeout", Duration.class)
            .invoke(locations, Duration.ofMillis(17));
        options.addStreamNode("gateway")
            .bind("inproc://session-seal-timeout-owner")
            .registerSession(TestSession.class);

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(
                     options, new ZLinkJavaBackendAdapterFactory())) {
            locations.getClass()
                .getMethod("setSessionRelocationSealTimeout", Duration.class)
                .invoke(locations, Duration.ofMillis(29));
            Object sessionOwner = runtime.sessionActors(
                "gateway", RoutingId.from("session-timeout"));
            Field timeout = sessionOwner.getClass()
                .getDeclaredField("sessionRelocationSealTimeout");
            timeout.setAccessible(true);
            assertEquals(Duration.ofMillis(17), timeout.get(sessionOwner));
        }
    }

    @Test
    void streamBuilderDoesNotExposeASecondTimeoutOwner() {
        assertThrows(NoSuchMethodException.class, () ->
            ZLinkStreamNodeBuilder.class.getMethod(
                "sessionRelocationSealTimeout", Duration.class));
    }

    public static final class TestSession implements ZLinkSession {
        @Override public ZLinkSessionContext context() { return null; }

        @Override public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
