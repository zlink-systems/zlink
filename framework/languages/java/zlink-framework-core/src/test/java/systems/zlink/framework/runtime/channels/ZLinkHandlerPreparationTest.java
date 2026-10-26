package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

import java.lang.reflect.Constructor;
import java.util.Arrays;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkHandlerPreparationTest {
    @Test
    void preparedServicesConstructionAcceptsAnotherActivationImplementation() {
        var services = ZLinkHandlerActivator.services();
        var plan = services.prepareConstruction(CatalogHandler.class, java.util.Set.of(Known.class));
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
            public PreparedConstruction prepareConstruction(Class<?> type, java.util.Set<Class<?>> keys) {
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
            implements systems.zlink.framework.spots.ZLinkSpot<systems.zlink.framework.actors.ZLinkActor> {
        public DualConstructorSpot() {}
        public DualConstructorSpot(systems.zlink.framework.spots.ZLinkSpotContext context) {
            throw new AssertionError("Standalone reflection must use the no-arg constructor");
        }
        public systems.zlink.framework.spots.ZLinkSpotContext context() { return null; }
        public CompletionStage<Void> onJoinedActor(systems.zlink.framework.actors.ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        public CompletionStage<Void> onLeaveActor(systems.zlink.framework.actors.ZLinkActor actor) {
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
                if (type == Missing.class) missingLookups.incrementAndGet();
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
