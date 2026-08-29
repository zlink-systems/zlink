package systems.zlink.samples.kotlin.zoneworld.server.ops;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.lang.reflect.Proxy;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.streams.ZLinkSessionClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionSendCall;

final class OpsConsoleRegistryTest {
    @Test
    void staleSessionSubmitFailureDoesNotHideNotificationFromLiveSession() {
        OpsConsoleRegistry registry = new OpsConsoleRegistry();
        AtomicInteger liveSubmissions = new AtomicInteger();
        registry.add(session(() -> {
            throw new IllegalStateException("stale session");
        }));
        registry.add(session(liveSubmissions::incrementAndGet));

        registry.broadcast(new Object());

        assertEquals(1, liveSubmissions.get());
    }

    private static ZLinkSessionContext session(Runnable submit) {
        ZLinkSessionSendCall call = (ZLinkSessionSendCall) Proxy.newProxyInstance(
            ZLinkSessionSendCall.class.getClassLoader(),
            new Class<?>[] {ZLinkSessionSendCall.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "submit" -> {
                    submit.run();
                    yield CompletableFuture.completedFuture(null);
                }
                case "metadata", "compress", "timeout" -> proxy;
                default -> throw new UnsupportedOperationException(method.getName());
            });
        ZLinkSessionClient client = (ZLinkSessionClient) Proxy.newProxyInstance(
            ZLinkSessionClient.class.getClassLoader(),
            new Class<?>[] {ZLinkSessionClient.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "send" -> call;
                default -> throw new UnsupportedOperationException(method.getName());
            });
        return (ZLinkSessionContext) Proxy.newProxyInstance(
            ZLinkSessionContext.class.getClassLoader(),
            new Class<?>[] {ZLinkSessionContext.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "client" -> client;
                case "sessionId" -> "test-session";
                case "routingId", "localAddr", "remoteAddr" -> Optional.empty();
                case "close" -> CompletableFuture.completedFuture(null);
                default -> throw new UnsupportedOperationException(method.getName());
            });
    }
}
