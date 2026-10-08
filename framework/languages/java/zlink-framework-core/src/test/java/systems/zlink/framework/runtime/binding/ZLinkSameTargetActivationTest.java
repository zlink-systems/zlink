package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;

import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSameTargetActivationTest {
    @Test
    void pendingOperationsEnterTheQueueInTargetArrivalOrder() {
        var ready = new CompletableFuture<Void>();
        var created = new AtomicInteger();
        var admitted = new ArrayList<Integer>();
        var registry = new ZLinkJavaInstanceSpotRegistry();
        registry.register(
                "room",
                (id, generation) -> {
                    created.incrementAndGet();
                    return (ZLinkBackendSpot)
                            Proxy.newProxyInstance(
                                    ZLinkBackendSpot.class.getClassLoader(),
                                    new Class<?>[] {ZLinkBackendSpot.class},
                                    (proxy, method, arguments) ->
                                            switch (method.getName()) {
                                                case "lifecycleGeneration" -> generation;
                                                case "close" -> null;
                                                default ->
                                                        throw new AssertionError(method.getName());
                                            });
                },
                (type, id, generation, spot) -> ready);
        var event = registry.activate("spot", "room", 1, Long.MAX_VALUE, spot -> admitted.add(1));
        var request = registry.activate("spot", "room", 1, Long.MAX_VALUE, spot -> admitted.add(2));
        var later = registry.activate("spot", "room", 1, Long.MAX_VALUE, spot -> admitted.add(3));
        assertTrue(admitted.isEmpty());
        ready.complete(null);
        assertSame(
                event.toCompletableFuture().join().spot(),
                request.toCompletableFuture().join().spot());
        assertSame(
                event.toCompletableFuture().join().spot(),
                later.toCompletableFuture().join().spot());
        assertEquals(1, created.get());
        assertEquals(List.of(1, 2, 3), admitted);
        registry.closeAll();
    }
}
