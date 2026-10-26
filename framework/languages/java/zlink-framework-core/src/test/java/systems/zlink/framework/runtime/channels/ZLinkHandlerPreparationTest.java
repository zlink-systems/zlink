package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.util.AbstractMap;
import java.util.AbstractSet;
import java.util.Arrays;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkHandlerPreparationTest {
    @Test
    @SuppressWarnings("unchecked")
    void dispatchConstructionDoesNotHashTheCatalogServiceKeys() throws Exception {
        var services = ZLinkHandlerActivator.services();
        var known = new Known();
        services.add(Known.class, known);
        for (int dimensions = 1; dimensions <= 64; dimensions++) {
            Object extra = Array.newInstance(Known.class, new int[dimensions]);
            services.add(extra.getClass(), extra);
        }
        var field = ZLinkHandlerActivator.MutableServices.class.getDeclaredField("serviceIndex");
        field.setAccessible(true);
        var index = (Map<Class<?>, Object>) field.get(services);
        var hashes = new AtomicInteger();
        var keys = new AbstractSet<Class<?>>() {
            public Iterator<Class<?>> iterator() { return index.keySet().iterator(); }
            public int size() { return index.size(); }
            public boolean contains(Object key) { return index.containsKey(key); }
            public int hashCode() {
                hashes.incrementAndGet();
                return super.hashCode();
            }
        };
        field.set(services, new AbstractMap<Class<?>, Object>() {
            public Set<Map.Entry<Class<?>, Object>> entrySet() {
                return index.entrySet();
            }
            public Object get(Object key) { return index.get(key); }
            public Set<Class<?>> keySet() { return keys; }
        });
        keys.hashCode();
        assertEquals(1, hashes.get());
        services.prepare(CatalogHandler.class);
        hashes.set(0);
        for (int dispatch = 0; dispatch < 8; dispatch++) {
            assertSame(known, ((CatalogHandler) services.create(CatalogHandler.class)).known);
            try (var activation = services.openActivation()) {
                assertSame(known, ((CatalogHandler) activation.create(CatalogHandler.class)).known);
                assertSame(known, ((CatalogHandler) activation.create(
                        CatalogHandler.class, ignored -> null)).known);
            }
            try (var owner = new ZLinkHandlerInstanceOwner(services)) {
                assertSame(known, ((CatalogHandler) owner.instance(CatalogHandler.class)).known);
            }
        }
        assertEquals(0, hashes.get(), "dispatch must look up construction plans by handler type only");
    }

    @Test
    void preparedServicesConstructionAcceptsAnotherActivationImplementation() {
        var services = ZLinkHandlerActivator.services();
        var plan = services.prepareConstruction(CatalogHandler.class, Set.of(Known.class));
        var dependency = new Known();
        try (var activation = ZLinkHandlerActivator.reflection().openActivation()) {
            assertSame(dependency, ((CatalogHandler) plan.create(activation, type -> dependency)).known);
        }
    }

    @Test
    void nestedScopesReuseTheParentCatalogConstructionPlan() {
        var compilations = new AtomicInteger();
        var reflection = ZLinkHandlerActivator.reflection();
        var adapter = new ZLinkHandlerActivator() {
            public Object create(Class<?> type) { return reflection.create(type); }
            public PreparedConstruction prepareConstruction(Class<?> type, Set<Class<?>> keys) {
                compilations.incrementAndGet();
                return reflection.prepareConstruction(type, keys);
            }
        };
        var parent = ZLinkHandlerActivator.services(adapter);
        var known = new Known();
        parent.add(Known.class, known);
        parent.prepare(CatalogHandler.class);
        for (int index = 0; index < 3; index++) {
            var scoped = ZLinkHandlerActivator.services(parent);
            var current = new Known();
            scoped.add(Known.class, current);
            scoped.prepare(CatalogHandler.class);
            try (var activation = scoped.openActivation()) {
                assertSame(current, ((CatalogHandler) activation.create(CatalogHandler.class)).known);
            }
        }
        assertEquals(1, compilations.get());
    }

    @Test
    void standaloneReflectionUsesItsNoArgPlanWithoutAnActivationContext() {
        var reflection = ZLinkHandlerActivator.reflection();
        reflection.prepare(DualConstructorSpot.class);
        assertNotNull(reflection.create(DualConstructorSpot.class));
        assertNotNull(reflection.findService(DualConstructorSpot.class));
    }

    public static final class DualConstructorSpot
            implements ZLinkSpot<ZLinkActor> {
        public DualConstructorSpot() {}
        public DualConstructorSpot(ZLinkSpotContext context) {
            throw new AssertionError("Standalone reflection must use the no-arg constructor");
        }
        public ZLinkSpotContext context() { return null; }
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void constructionPlansBelongToTheirRegistrationCatalog() {
        var first = ZLinkHandlerActivator.services();
        var second = ZLinkHandlerActivator.services();
        var known = new Known();
        first.add(Known.class, known);
        first.prepare(CatalogHandler.class);
        second.prepare(CatalogHandler.class);
        assertSame(known, ((CatalogHandler) first.create(CatalogHandler.class)).known);
        assertNull(((CatalogHandler) second.create(CatalogHandler.class)).known);
    }

    @Test
    void preparationRejectsMissingDependenciesWithoutRunningConstructors() {
        var services = ZLinkHandlerActivator.services();
        assertThrows(
                ZLinkConfigurationException.class,
                () -> services.prepare(PreparedConstruction.class));
    }

    @Test
    void explicitApplicationFactoryRemainsTheConstructionProcedure() {
        var expected = new CatalogHandler();
        var calls = new AtomicInteger();
        var services = ZLinkHandlerActivator.services(type -> {
            calls.incrementAndGet();
            return expected;
        });
        services.prepare(CatalogHandler.class);
        assertEquals(0, calls.get());
        assertSame(expected, services.create(CatalogHandler.class));
        assertEquals(1, calls.get());
    }

    @Test
    void nestedCatalogActivationUsesThePreparedKeys() {
        var parent = ZLinkHandlerActivator.services();
        var known = new Known();
        parent.add(Known.class, known);
        var child = ZLinkHandlerActivator.services(parent);
        child.prepare(CatalogHandler.class);
        try (var activation = child.openActivation()) {
            assertSame(known, ((CatalogHandler) activation.create(CatalogHandler.class)).known);
        }
    }

    public static final class CatalogHandler {
        final Known known;
        public CatalogHandler(Known known) { this.known = known; }
        public CatalogHandler() { this.known = null; }
    }

    @Test
    void mutableServicesReusesThePreparedConstructorWithoutMissingDependencyLookups() {
        var missingLookups = new AtomicInteger();
        var reflection = ZLinkHandlerActivator.reflection();
        var fallback = new ZLinkHandlerActivator() {
            public boolean isServiceAvailable(Constructor<?> constructor, int index) {
                Class<?> type = constructor.getParameterTypes()[index];
                return Arrays.stream(type.getConstructors())
                        .anyMatch(candidate -> candidate.getParameterCount() == 0);
            }
            public Object create(Class<?> type) { return reflection.create(type); }
            public Object findService(Class<?> type) {
                if (type == Missing.class) {
                    missingLookups.incrementAndGet();
                }
                return reflection.findService(type);
            }
        };
        var services = ZLinkHandlerActivator.services(fallback);
        services.add(Known.class, new Known());
        services.prepare(PreparedConstruction.class);
        missingLookups.set(0);
        for (int index = 0; index < 3; index++) {
            assertNotNull(services.create(PreparedConstruction.class));
        }
        assertEquals(0, missingLookups.get());
    }

    public static final class Known {}
    public interface Missing {}
    public static final class PreparedConstruction {
        public PreparedConstruction(Known known, Missing missing) {
            throw new AssertionError("Rejected constructor executed");
        }
        public PreparedConstruction(Known known) {}
    }

    @Test
    void configuredRegistrationSelectsInvocationDuringConstruction() {
        var declaration =
                new ChannelRouteSendHandlerRegistration(Handler.class, String.class, "message");
        assertNotNull(declaration.invocation());
    }

    @Test
    void declaredPrimitivePayloadBindsItsBoxedInvocationValue() {
        var registration =
                new ChannelRequestHandlerRegistration(
                        PrimitiveHandler.class,
                        int.class,
                        int.class,
                        "number",
                        java.util.List.of());
        assertEquals(
                7,
                registration
                        .invocation()
                        .invoke(new PrimitiveHandler(), 7, null)
                        .toCompletableFuture()
                        .join());
    }

    public static final class PrimitiveHandler {
        public int handle(int message, systems.zlink.framework.ZLinkMessageContext context) {
            return message;
        }
    }

    @Test
    void annotatedMethodReceivesOnlyItsDeclaredArguments() throws Exception {
        var registration =
                new ChannelRequestHandlerRegistration(
                        SingleParameterHandler.class,
                        SingleParameterHandler.class.getMethod("handle", String.class),
                        String.class,
                        String.class,
                        "message",
                        java.util.List.of());
        assertEquals(
                "message",
                registration
                        .invocation()
                        .invoke(new SingleParameterHandler(), "message", null)
                        .toCompletableFuture()
                        .join());
    }

    public static final class SingleParameterHandler {
        public String handle(String message) {
            return message;
        }
    }

    @Test
    void interfaceMethodIsSelectedBeforeDispatchAndRetainedAcrossPacketNaming() {
        var registration =
                new ChannelRouteSendHandlerRegistration(
                        Handler.class, String.class, null, java.util.List.of());
        assertNotNull(registration.invocation().method());
        assertSame(
                registration.invocation().method(),
                registration.withPacketName("message").invocation().method());
    }

    public static final class Handler {
        public CompletionStage<Void> handle(String message, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
