package systems.zlink.framework.runtime.internal.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.lang.reflect.Field;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class ZLinkHandlerActivatorTest {
    @Test
    void mutableServicesBuildsAssignableLookupIndexAtRegistration() throws Exception {
        ZLinkHandlerActivator.MutableServices services =
            ZLinkHandlerActivator.services();
        TestRuntimeService service = new TestRuntimeService();

        services.add(TestRuntimeService.class, service);

        Field field = ZLinkHandlerActivator.MutableServices.class
            .getDeclaredField("serviceIndex");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<Class<?>, Object> index =
            (Map<Class<?>, Object>) field.get(services);
        assertSame(service, index.get(TestRuntimeService.class));
        assertSame(service, index.get(RuntimeService.class));
        assertSame(service, index.get(Object.class));
    }

    @Test
    void mutableServicesPreservesAssignableLookupForInterfaceRegistrations()
        throws Exception {
        ZLinkHandlerActivator.MutableServices services =
            ZLinkHandlerActivator.services();
        TestRuntimeService service = new TestRuntimeService();

        services.add(RuntimeService.class, service);

        Field field = ZLinkHandlerActivator.MutableServices.class
            .getDeclaredField("serviceIndex");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<Class<?>, Object> index =
            (Map<Class<?>, Object>) field.get(services);
        assertSame(service, index.get(RuntimeService.class));
        assertSame(service, index.get(Object.class));
    }

    @Test
    void reflectionPreparationCachesConstructorWithoutCreatingHandler()
        throws Exception {
        CountingHandler.instances.set(0);
        ZLinkHandlerActivator activator = ZLinkHandlerActivator.reflection();

        activator.prepare(CountingHandler.class);

        assertEquals(0, CountingHandler.instances.get());
        PublicConstructorPlan prepared =
            PublicConstructorPlan.forType(CountingHandler.class);

        activator.create(CountingHandler.class);
        assertEquals(1, CountingHandler.instances.get());
        assertSame(prepared, PublicConstructorPlan.forType(CountingHandler.class));
    }

    private interface RuntimeService {
    }

    private static final class TestRuntimeService implements RuntimeService {
    }

    private static final class CountingHandler {
        private static final AtomicInteger instances = new AtomicInteger();

        public CountingHandler() {
            instances.incrementAndGet();
        }
    }
}
