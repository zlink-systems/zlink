package systems.zlink.framework.runtime.internal.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkHandlerActivatorTest {
    @Test
    void fallbackDependencyConstructionFailureIsPreserved() {
        IllegalStateException failure = new IllegalStateException("dependency construction failed");
        ZLinkHandlerActivator.MutableServices services =
                ZLinkHandlerActivator.services(
                        type -> {
                            if (type == MissingService.class) throw failure;
                            return null;
                        });
        services.add(TestRuntimeService.class, new TestRuntimeService());

        assertSame(
                failure,
                assertThrows(
                        IllegalStateException.class,
                        () -> services.create(NeedsRuntimeService.class)));
    }

    public static final class NeedsRuntimeService {
        public NeedsRuntimeService(TestRuntimeService runtime, MissingService dependency) {}
    }

    private interface MissingService {}

    @Test
    void optionalReflectionServicePreservesConstructorFailureAndReturnsNullOnlyForAbsence() {
        ZLinkHandlerActivator services = ZLinkHandlerActivator.services();
        org.junit.jupiter.api.Assertions.assertNull(services.findService(RuntimeService.class));
        var failure =
                assertThrows(
                        systems.zlink.framework.errors.ZLinkConfigurationException.class,
                        () -> services.findService(ThrowingService.class));
        assertSame(ThrowingService.FAILURE, failure.getCause().getCause());
        assertThrows(
                systems.zlink.framework.errors.ZLinkConfigurationException.class,
                () -> services.create(RuntimeService.class));
    }

    public static final class ThrowingService {
        private static final IllegalStateException FAILURE =
                new IllegalStateException("ctor failed");

        public ThrowingService() {
            throw FAILURE;
        }
    }

    @Test
    void mutableServicesResolvesAssignableTypesAfterRegistration() {
        AtomicInteger fallbackCalls = new AtomicInteger();
        ZLinkHandlerActivator.MutableServices services =
                ZLinkHandlerActivator.services(
                        type -> {
                            fallbackCalls.incrementAndGet();
                            return null;
                        });
        TestRuntimeService service = new TestRuntimeService();

        services.add(TestRuntimeService.class, service);

        assertSame(service, services.findService(TestRuntimeService.class));
        assertSame(service, services.findService(RuntimeService.class));
        assertSame(service, services.findService(Object.class));
        assertEquals(0, fallbackCalls.get());
    }

    @Test
    void mutableServicesPreservesAssignableLookupForInterfaceRegistrations() {
        AtomicInteger fallbackCalls = new AtomicInteger();
        ZLinkHandlerActivator.MutableServices services =
                ZLinkHandlerActivator.services(
                        type -> {
                            fallbackCalls.incrementAndGet();
                            return null;
                        });
        TestRuntimeService service = new TestRuntimeService();

        services.add(RuntimeService.class, service);

        assertSame(service, services.findService(RuntimeService.class));
        assertSame(service, services.findService(Object.class));
        assertEquals(0, fallbackCalls.get());
    }

    @Test
    void reflectionPreparationCachesConstructorWithoutCreatingHandler() throws Exception {
        CountingHandler.instances.set(0);
        ZLinkHandlerActivator activator = ZLinkHandlerActivator.reflection();

        activator.prepare(CountingHandler.class);

        assertEquals(0, CountingHandler.instances.get());
        PublicConstructorPlan prepared = PublicConstructorPlan.forType(CountingHandler.class);

        activator.create(CountingHandler.class);
        assertEquals(1, CountingHandler.instances.get());
        assertSame(prepared, PublicConstructorPlan.forType(CountingHandler.class));
    }

    private interface RuntimeService {}

    private static final class TestRuntimeService implements RuntimeService {}

    private static final class CountingHandler {
        private static final AtomicInteger instances = new AtomicInteger();

        public CountingHandler() {
            instances.incrementAndGet();
        }
    }
}
