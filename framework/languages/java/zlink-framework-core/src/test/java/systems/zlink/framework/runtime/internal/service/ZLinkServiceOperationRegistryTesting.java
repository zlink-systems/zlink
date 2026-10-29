package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicLong;

public final class ZLinkServiceOperationRegistryTesting {
    private ZLinkServiceOperationRegistryTesting() {}

    public static Object pendingReceiver(ZLinkServiceOperationRegistry registry) {
        return registry.pendingReceiverForTest();
    }

    public static boolean pendingRegistered(ZLinkServiceOperationRegistry registry) {
        return registry.pendingRegisteredForTest();
    }

    public static final class RegistrationProbe {
        private final AtomicLong count = new AtomicLong();
        private final ZLinkServiceOperationRegistry registry;

        public long count() {
            return count.get();
        }

        public ZLinkServiceOperationRegistry registry() {
            return registry;
        }

        public RegistrationProbe(ZLinkStateLane lane, Throwable closeFailure) {
            var pending =
                    new ConcurrentHashMap<UUID, ZLinkServiceOperationRegistry.Entry<?>>() {
                        @Override
                        public ZLinkServiceOperationRegistry.Entry<?> put(
                                UUID id, ZLinkServiceOperationRegistry.Entry<?> entry) {
                            assertTrue(
                                    entry.registeredForTest(),
                                    "dispatcher registration must precede hash publication");
                            count.incrementAndGet();
                            return super.put(id, entry);
                        }
                    };
            assertTrue(pending.isEmpty());
            registry =
                    new ZLinkServiceOperationRegistry(
                            lane,
                            Thread.ofVirtual().name("zlink-test-maintenance").factory(),
                            closeFailure,
                            () -> new TimeoutException("service operation timed out"),
                            System::nanoTime,
                            pending);
        }
    }
}
