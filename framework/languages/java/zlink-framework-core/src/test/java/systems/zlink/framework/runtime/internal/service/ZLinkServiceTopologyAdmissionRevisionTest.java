package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.List;
import java.util.stream.Stream;

final class ZLinkServiceTopologyAdmissionRevisionTest {
    static Stream<Arguments> revisionCases() {
        return Stream.of(
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_ADMIT,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_UPDATE,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        true),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        2,
                        ZLinkServiceNodeDescriptor.State.PREPARING,
                        false,
                        true),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        2,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        1,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_HELLO,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        true,
                        false),
                Arguments.of(
                        ServiceWireConstants.COMMAND_UPDATE,
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        true,
                        false));
    }

    @ParameterizedTest
    @MethodSource("revisionCases")
    void sameLifecycleRevisionIsCommandOwned(
            int command,
            long revision,
            ZLinkServiceNodeDescriptor.State state,
            boolean changedEndpoint,
            boolean accepted) {
        for (String connection : List.of("route", "replacement")) {
            var registry =
                    new ZLinkServiceTopologyRegistry(
                            descriptor(
                                    "local", 1, ZLinkServiceNodeDescriptor.State.PREPARING, false));
            var original = descriptor("peer", 2, ZLinkServiceNodeDescriptor.State.PREPARING, false);
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    registry.admit(original, "route", ServiceWireConstants.COMMAND_HELLO));
            var incoming = descriptor("peer", revision, state, changedEndpoint);
            var result = registry.admit(incoming, connection, command);
            assertEquals(
                    accepted,
                    result == ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    "command=" + command + " connection=" + connection);
            assertEquals(
                    accepted ? incoming : original,
                    registry.peer(original.nodeRoutingId()).orElseThrow().descriptor());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void updateCannotEstablishAdmission(boolean knownPeer) {
        var registry =
                new ZLinkServiceTopologyRegistry(
                        descriptor("local", 1, ZLinkServiceNodeDescriptor.State.PREPARING, false));
        var original = descriptor("peer", 2, ZLinkServiceNodeDescriptor.State.PREPARING, false);
        if (knownPeer) {
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    registry.admit(original, "route", ServiceWireConstants.COMMAND_HELLO));
        }
        var incoming =
                descriptor(
                        "peer",
                        3,
                        ZLinkServiceNodeDescriptor.State.SERVING,
                        false,
                        knownPeer ? 8 : 7);
        assertEquals(
                ZLinkServiceTopologyRegistry.AdmissionResult.STALE_DESCRIPTOR,
                registry.admit(incoming, "replacement", ServiceWireConstants.COMMAND_UPDATE));
        if (knownPeer) {
            var peer = registry.peer(original.nodeRoutingId()).orElseThrow();
            assertEquals(original, peer.descriptor());
            assertEquals("route", peer.connectionId());
        } else {
            assertTrue(registry.peer(incoming.nodeRoutingId()).isEmpty());
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(
            String id, long revision, ZLinkServiceNodeDescriptor.State state, boolean changed) {
        return descriptor(id, revision, state, changed, 7);
    }

    private static ZLinkServiceNodeDescriptor descriptor(
            String id,
            long revision,
            ZLinkServiceNodeDescriptor.State state,
            boolean changed,
            long lifecycleGeneration) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                RoutingId.from(id),
                lifecycleGeneration,
                revision,
                "inproc://" + id + (changed ? "-changed" : ""),
                List.of(),
                state,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                1,
                1,
                0,
                0,
                0);
    }
}
