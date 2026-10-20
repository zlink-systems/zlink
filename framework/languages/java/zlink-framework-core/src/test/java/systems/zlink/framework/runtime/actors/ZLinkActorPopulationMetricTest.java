package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.util.*;

final class ZLinkActorPopulationMetricTest {
    @Test
    void actorPopulationUsesTheActorsCapturedMeshRatherThanTheMutableRuntimeMesh()
            throws Exception {
        var node =
                (ZLinkInternalSpotNode)
                        java.lang.reflect.Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("routingId"))
                                        return RoutingId.from("node");
                                    throw new AssertionError(method);
                                });
        var runtime =
                new ZLinkActorRuntime(
                        node,
                        Map.of(),
                        java.time.Duration.ofSeconds(5),
                        new ZLinkJsonMessageSerializer());
        runtime.setMeshName("actor-mesh");
        var context =
                runtime
                .new DefaultActorContext(
                        new ZLinkBackendActorRef(RoutingId.from("node"), "actor", 1));
        var mesh = ZLinkActorRuntime.class.getDeclaredField("meshName");
        mesh.setAccessible(true);
        mesh.set(runtime, null);
        var field = ZLinkActorRuntime.class.getDeclaredField("actorRegistry");
        field.setAccessible(true);
        var registry = field.get(runtime);
        var register =
                registry.getClass()
                        .getDeclaredMethod(
                                "register",
                                String.class,
                                String.class,
                                ZLinkActor.class,
                                ZLinkActorRuntime.DefaultActorContext.class);
        register.setAccessible(true);
        var remove = registry.getClass().getDeclaredMethod("remove", String.class);
        remove.setAccessible(true);
        ZLinkActor actor =
                new ZLinkActor() {
                    public ZLinkActorContext context() {
                        return context;
                    }
                };
        try (var metrics = ZLinkRuntimeMetricProbe.install()) {
            try {
                register.invoke(registry, "actor", "type", actor, context);
                assertEquals(
                        1,
                        metrics.observable("zlink.actor.count", Map.of("mesh_name", "actor-mesh")));
            } finally {
                runtime.setMeshName("later-mesh");
                remove.invoke(registry, "actor");
            }
            assertEquals(
                    0, metrics.observable("zlink.actor.count", Map.of("mesh_name", "actor-mesh")));
        } finally {
            runtime.closeAsync().toCompletableFuture().join();
        }
    }
}
