package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

import java.time.Duration;
import java.util.concurrent.CompletionException;

final class ZLinkJavaRawMeshNodeSessionOwnerDescriptorTest {
    @Test
    void remoteBindWithoutASessionOwnerDescriptorIsUnavailable() {
        String endpoint = "inproc://bind-no-descriptor-" + System.nanoTime();
        RoutingId sessionNodeRid = RoutingId.from("bind-no-descriptor-session-node");
        RoutingId actorNodeRid = RoutingId.from("bind-no-descriptor-actor-node");
        try (var context = Zlink.createContext();
                var sessionNode = new ZLinkJavaRawMeshNode(context, "mesh");
                var actorNode = new ZLinkJavaRawMeshNode(context, "mesh")) {
            sessionNode.setRoutingId(sessionNodeRid);
            sessionNode.setBind(endpoint + "-session");
            actorNode.setRoutingId(actorNodeRid);
            actorNode.setBind(endpoint);
            sessionNode.start();
            actorNode.start();
            // The Actor owner has no descriptor source, so it has no admitted, lease-verified
            // descriptor of the Session owner.
            sessionNode.connectPeer(endpoint, actorNodeRid);
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = actorNode.spotNode().createActor("player", create);
            }
            ((ZLinkJavaRawSpotNode) sessionNode.spotNode()).rememberActorAuthority(actor, 3, 1);

            // Session-Actor binding §4: without that descriptor the bind is Unavailable.
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    sessionNode
                                            .bindRemoteStreamSession(
                                                    RoutingId.from("bind-no-descriptor-session"),
                                                    actor,
                                                    3,
                                                    1,
                                                    true,
                                                    Duration.ofSeconds(5))
                                            .toCompletableFuture()
                                            .join());
            var rejected = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, rejected.kind());
        }
    }
}
