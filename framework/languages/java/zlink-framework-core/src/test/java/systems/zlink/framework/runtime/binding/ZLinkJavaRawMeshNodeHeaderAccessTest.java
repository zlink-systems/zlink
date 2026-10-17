package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.AbstractList;
import java.util.List;

final class ZLinkJavaRawMeshNodeHeaderAccessTest {
    private static final RoutingId SOURCE = RoutingId.from("header-peer");

    @Test
    void nonInfrastructureHeaderIsCopiedOnceBeforeUnadmittedSourceIsDropped() throws Exception {
        try (var context = Zlink.createContext();
                var node = node(context);
                var record = new Record(prefix(ServiceWireConstants.COMMAND_NODE_SEND))) {
            dispatch(node, record);
            record.assertClosed();
            assertEquals(1, record.frames.headerReads);
        }
    }

    @Test
    void emptyRecordIsDroppedWithoutReadingAHeader() throws Exception {
        try (var context = Zlink.createContext();
                var node = node(context);
                var record = new Record()) {
            dispatch(node, record);
            record.assertClosed();
            assertEquals(0, record.frames.headerReads);
        }
    }

    @Test
    void shortHeaderIsDroppedAfterOneRead() throws Exception {
        try (var context = Zlink.createContext();
                var node = node(context);
                var record = new Record(new byte[] {(byte) ServiceWireConstants.MAGIC_0})) {
            dispatch(node, record);
            record.assertClosed();
            assertEquals(1, record.frames.headerReads);
        }
    }

    @Test
    void invalidMagicIsDroppedAndOwnedMessageIsClosed() throws Exception {
        byte[] head = prefix(ServiceWireConstants.COMMAND_NODE_SEND);
        head[0] = 0;
        try (var context = Zlink.createContext();
                var node = node(context);
                var record = new Record(head)) {
            dispatch(node, record);
            record.assertClosed();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void infrastructureRejectKeepsItsShapeValidationAndClosesAllOwnedParts(boolean extraPart)
            throws Exception {
        byte[] reject = new ZLinkServiceM6AWireCodec().encodeReject(1);
        try (var context = Zlink.createContext();
                var node = node(context);
                var record = extraPart ? new Record(reject, new byte[] {1}) : new Record(reject)) {
            dispatch(node, record);
            record.assertClosed();
            assertTrue(record.frames.headerReads > 0);
            assertEquals(
                    0, record.frames.nonHeaderReads, "invalid shape must not read extra parts");
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void updateOnlyChangesTheAdmittedRouteDescriptor(boolean admittedRoute) throws Exception {
        var local = descriptor(RoutingId.from("header-local"), 1);
        var topology = new ZLinkServiceTopologyRegistry(local);
        topology.admit(
                descriptor(SOURCE, 1),
                admittedRoute ? "0" : "1",
                ServiceWireConstants.COMMAND_HELLO);
        byte[] update =
                new ZLinkServiceM6AWireCodec()
                        .encodeAdmission(
                                ServiceWireConstants.COMMAND_UPDATE, descriptor(SOURCE, 2));
        try (var context = Zlink.createContext();
                var node = node(context);
                var record = new Record(update)) {
            set(node, "localDescriptor", local);
            set(node, "topology", topology);
            dispatch(node, record);
            record.assertClosed();
            assertEquals(
                    admittedRoute ? 2 : 1,
                    topology.peer(SOURCE).orElseThrow().descriptor().descriptorRevision());
            assertEquals(
                    admittedRoute ? "0" : "1", topology.peer(SOURCE).orElseThrow().connectionId());
        }
    }

    private static ZLinkJavaRawMeshNode node(Context context) throws Exception {
        var node = new ZLinkJavaRawMeshNode(context, "header");
        var local = descriptor(RoutingId.from("header-local"), 1);
        set(node, "localDescriptor", local);
        set(node, "topology", new ZLinkServiceTopologyRegistry(local));
        return node;
    }

    private static void dispatch(ZLinkJavaRawMeshNode node, Record record) throws Exception {
        var method =
                ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                        "dispatch", ZLinkJavaRawServicePort.Inbound.class);
        method.setAccessible(true);
        method.invoke(node, record.inbound);
    }

    private static void set(Object owner, String name, Object value) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(owner, value);
    }

    private static byte[] prefix(int command) {
        return new byte[] {
            (byte) ServiceWireConstants.MAGIC_0,
            (byte) ServiceWireConstants.MAGIC_1,
            (byte) ServiceWireConstants.WIRE_MAJOR,
            (byte) command,
            0
        };
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long revision) {
        return new ZLinkServiceNodeDescriptor(
                "header",
                rid,
                1,
                revision,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
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

    private static final class Record implements AutoCloseable {
        private final List<Message> messages;
        private final Received received;
        private final CopyingFrames frames;
        private final ZLinkJavaRawServicePort.Inbound inbound;

        private Record(byte[]... parts) {
            messages = java.util.Arrays.stream(parts).map(Message::from).toList();
            received = new Received(SOURCE, messages.toArray(Message[]::new));
            frames = new CopyingFrames(received, messages.size());
            inbound = new ZLinkJavaRawServicePort.Inbound(SOURCE, null, frames, received);
        }

        private void assertClosed() {
            assertThrows(IllegalStateException.class, received::parts);
            for (Message message : messages) {
                assertEquals(0, message.size(), "dispatch must close each owned native message");
            }
        }

        @Override
        public void close() {
            received.close();
        }
    }

    // Match RawServicePort.receiveNow: every indexed access materializes a fresh native part copy.
    private static final class CopyingFrames extends AbstractList<byte[]> {
        private final Received received;
        private final int count;
        private int headerReads;
        private int nonHeaderReads;

        private CopyingFrames(Received received, int count) {
            this.received = received;
            this.count = count;
        }

        @Override
        public byte[] get(int index) {
            if (index == 0) {
                headerReads++;
            } else {
                nonHeaderReads++;
            }
            return received.parts().get(index).toByteArray();
        }

        @Override
        public int size() {
            return count;
        }
    }
}
