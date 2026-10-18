package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/** Native Session route used by relocation tests across the runtime package boundary. */
public final class ZLinkNativeSessionRouteFixture implements AutoCloseable {
    private final systems.zlink.contracts.core.Context context;
    private final ZLinkJavaRawMeshNode target;
    private final ZLinkJavaRawMeshNode sessionOwner;
    private final CompletableFuture<Void> received = new CompletableFuture<>();
    private final CompletableFuture<RoutingId> requested = new CompletableFuture<>();

    public ZLinkNativeSessionRouteFixture() {
        context = Zlink.createContext();
        target = new ZLinkJavaRawMeshNode(context, "mesh");
        sessionOwner = new ZLinkJavaRawMeshNode(context, "mesh");
        target.setRoutingId(RoutingId.from("target-node"));
        target.setBind("tcp://127.0.0.1:0");
        sessionOwner.setRoutingId(RoutingId.from("session-owner"));
        sessionOwner.setBind("tcp://127.0.0.1:0");
        sessionOwner.setBoundSessionSendHandler(
                (source, generation, header, payload) -> {
                    received.complete(null);
                    return CompletableFuture.completedFuture(true);
                });
        target.start();
        sessionOwner.start();
        target.setPeerReconcileRequester(requested::complete);
    }

    public long sessionOwnerGeneration() {
        return sessionOwner.lifecycleGeneration();
    }

    public CompletableFuture<Void> routeReady() {
        return target.secureSessionOwnerRoute(
                        sessionOwner.routingId(), sessionOwner.lifecycleGeneration())
                .toCompletableFuture();
    }

    public void connect() throws Exception {
        if (!requested.get(5, TimeUnit.SECONDS).equals(sessionOwner.routingId()))
            throw new AssertionError("reconcile requested the wrong Session owner");
        target.connectPeer(sessionOwner.status().localEndpoint(), sessionOwner.routingId());
    }

    public void firstPush() throws Exception {
        var binding =
                new ZLinkJavaRawSpotNode.ActorSessionBinding(
                        sessionOwner.routingId(),
                        sessionOwner.lifecycleGeneration(),
                        "session-owner-id",
                        1,
                        RoutingId.from("session"),
                        new ZLinkBackendActorRef(target.routingId(), "actor", 1),
                        target.lifecycleGeneration(),
                        1,
                        1,
                        1,
                        true);
        try (var message = Message.from("first-push")) {
            target.sendBoundSession(binding, List.of(message))
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            received.get(5, TimeUnit.SECONDS);
        }
    }

    @Override
    public void close() {
        target.close();
        sessionOwner.close();
        context.close();
    }
}
