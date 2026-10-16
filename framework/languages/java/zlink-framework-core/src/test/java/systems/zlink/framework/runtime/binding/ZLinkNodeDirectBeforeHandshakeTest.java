package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionException;

final class ZLinkNodeDirectBeforeHandshakeTest {
    @Test
    void descriptorBackedObjectClientIsNotFoundBeforeHandshake() {
        observeBeforeHandshake(ZLinkMeshNodeObjectRole.CLIENT, true);
    }

    @Test
    void descriptorBackedObjectServerKeepsConnectionResultBeforeHandshake() {
        observeBeforeHandshake(ZLinkMeshNodeObjectRole.SERVER, true);
    }

    @Test
    void manualEndpointOnlyObjectClientKeepsConnectionResultBeforeHandshake() {
        observeBeforeHandshake(ZLinkMeshNodeObjectRole.CLIENT, false);
    }

    private void observeBeforeHandshake(ZLinkMeshNodeObjectRole role, boolean descriptorKnown) {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "repro-1826");
                var target = new ZLinkJavaRawMeshNode(context, "repro-1826")) {
            var targetRid = RoutingId.from("z-target");
            String endpoint = "inproc://repro-1826-target-" + role;
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setObjectRole(role);
            // Keep the actual target configured but unstarted: no HELLO can race this probe.
            source.setRoutingId(RoutingId.from("a-source"));
            source.setBind("inproc://repro-1826-source-" + role);
            source.start();
            if (descriptorKnown) {
                source.observePeerAdmissionExpectation(
                        targetRid,
                        endpoint,
                        1,
                        ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                        role);
            }
            source.connectPeer(endpoint, targetRid);
            assertEquals(MeshPeerState.CONNECTING, source.peers().getFirst().state());
            var classification = source.spotNode().classifyNodeSendTarget(targetRid);
            try (var packet = Message.from("repro")) {
                var submission = source.spotNode().sendToNode(targetRid, List.of(packet));
                assertTrue(submission.toCompletableFuture().isDone());
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () -> submission.toCompletableFuture().join());
                assertInstanceOf(ZlinkSubmitException.class, failure.getCause());
                assertEquals(
                        SubmitResult.NOT_CONNECTED,
                        ((ZlinkSubmitException) failure.getCause()).getResult());
            }
            if (descriptorKnown && role == ZLinkMeshNodeObjectRole.CLIENT) {
                assertEquals(Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND), classification);
                source.forgetPeerAdmissionExpectation(targetRid);
                assertTrue(source.spotNode().classifyNodeSendTarget(targetRid).isEmpty());
            } else {
                assertTrue(classification.isEmpty());
            }
        }
    }
}
