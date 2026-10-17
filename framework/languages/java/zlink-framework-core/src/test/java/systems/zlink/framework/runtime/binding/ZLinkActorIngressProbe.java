package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;

/** Observes real backend fences without replacing admission or dispatch decisions. */
public final class ZLinkActorIngressProbe {
    private static final int COMMAND_OFFSET = 3;

    public record Ingress(
            ZLinkServiceM6BWireCodec.ActorRouteFence forwarded,
            ZLinkServiceM6BWireCodec.ActorRouteFence received,
            long currentAuthority,
            long currentLease,
            boolean relocationOwnerAccepted) {}

    public final CompletableFuture<Void> forwarding = new CompletableFuture<>();
    public CompletableFuture<Void> forwardRelease = CompletableFuture.completedFuture(null);
    public final CompletableFuture<Void> published = new CompletableFuture<>();

    private final String actorId;

    public ZLinkActorIngressProbe(String actorId) {
        this.actorId = actorId;
    }

    public final CompletableFuture<Ingress> ingress = new CompletableFuture<>();
    private final AtomicReference<ZLinkServiceM6BWireCodec.ActorRouteFence> forwarded =
            new AtomicReference<>();

    public ZLinkBackendAdapterProvider provider() {
        var delegate = new ZLinkJavaBackendAdapterFactory();
        return (ZLinkBackendAdapterProvider)
                Proxy.newProxyInstance(
                        ZLinkBackendAdapterProvider.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendAdapterProvider.class},
                        (proxy, method, args) -> {
                            Object result = invoke(method, delegate, args);
                            if (!method.getName().equals("createMeshAdapter")) return result;
                            var adapter = (ZLinkMeshBackendAdapter) result;
                            return (ZLinkMeshBackendAdapter)
                                    (context, name) ->
                                            observeMesh(adapter.createMeshNode(context, name));
                        });
    }

    private ZLinkInternalMeshNode observeMesh(ZLinkInternalMeshNode mesh) {
        ZLinkInternalSpotNode observed = observeSpot(mesh.spotNode());
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, args) -> {
                            if (method.getName().equals("spotNode")) return observed;
                            if (method.getName().equals("setCanonicalRelocationControlHandler")
                                    && args[0] != null) {
                                var handler =
                                        (ZLinkInternalMeshNode.CanonicalRelocationControlHandler)
                                                args[0];
                                args =
                                        new Object[] {
                                            (ZLinkInternalMeshNode
                                                            .CanonicalRelocationControlHandler)
                                                    (source, sequence, command) ->
                                                            handler.handle(
                                                                            source, sequence,
                                                                            command)
                                                                    .whenComplete(
                                                                            (reply, failure) -> {
                                                                                // CUTOVER
                                                                                // completion
                                                                                // includes target
                                                                                // publication and
                                                                                // stage release.
                                                                                if (failure == null
                                                                                        && command[
                                                                                                        COMMAND_OFFSET]
                                                                                                == ServiceWireConstants
                                                                                                        .COMMAND_RELOCATION_CUTOVER)
                                                                                    published
                                                                                            .complete(
                                                                                                    null);
                                                                            })
                                        };
                            }
                            return invoke(method, mesh, args);
                        });
    }

    private ZLinkInternalSpotNode observeSpot(ZLinkInternalSpotNode raw) {
        return (ZLinkInternalSpotNode)
                Proxy.newProxyInstance(
                        ZLinkInternalSpotNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalSpotNode.class},
                        (proxy, method, args) -> {
                            if (method.getName().equals("forwardMessageFollowActor")) {
                                forwarded.set((ZLinkServiceM6BWireCodec.ActorRouteFence) args[1]);
                                forwarding.complete(null);
                                Object[] captured = args;
                                return forwardRelease.thenCompose(
                                        ignored -> forward(method, raw, captured));
                            }
                            if (method.getName().equals("setRelocationStagingIngressHandler")
                                    && args[0] != null) {
                                args = new Object[] {observeStaging(raw, args[0])};
                            }
                            return invoke(method, raw, args);
                        });
    }

    private Object observeStaging(ZLinkInternalSpotNode raw, Object handler) {
        var type = ZLinkInternalSpotNode.RelocationStagingIngressHandler.class;
        return Proxy.newProxyInstance(
                type.getClassLoader(),
                new Class<?>[] {type},
                (proxy, method, args) -> {
                    Object result = invoke(method, handler, args);
                    if (method.getName().equals("handleActor")) {
                        var header = (ZLinkServiceM6BWireCodec.ActorMessage) args[1];
                        if (!header.request()
                                && header.target().actor().actorId().equals(actorId)
                                && header.messageFollowHopCount() > 0) {
                            ingress.complete(
                                    new Ingress(
                                            forwarded.get(),
                                            header.target(),
                                            raw.actorAuthorityOwnerGeneration(
                                                    header.target().actor()),
                                            raw.actorAuthorityOwnerLeaseGeneration(
                                                    header.target().actor()),
                                            (boolean) result));
                        }
                    }
                    return result;
                });
    }

    @SuppressWarnings("unchecked")
    private static CompletionStage<List<Message>> forward(
            Method method, Object raw, Object[] args) {
        try {
            return (CompletionStage<List<Message>>) method.invoke(raw, args);
        } catch (InvocationTargetException failure) {
            return CompletableFuture.failedFuture(failure.getCause());
        } catch (IllegalAccessException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private static Object invoke(Method method, Object target, Object[] args) throws Throwable {
        try {
            return method.invoke(target, args);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }
}
