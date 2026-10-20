package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.io.IOException;
import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.instruction.InvokeInstruction;
import java.lang.constant.ClassDesc;
import java.lang.constant.MethodTypeDesc;
import java.lang.reflect.InvocationTargetException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/** Counts executed packet probes without changing their comparison or the transport path. */
public final class ZLinkJavaRawMeshNodeEnvelopeProbeTest {
    private static final AtomicInteger probes = new AtomicInteger();
    private static final AtomicInteger textDecodes = new AtomicInteger();
    private static final AtomicLong decodedBytes = new AtomicLong();

    @Test
    void serialEnvelopesDoNotMaterializeTextForTheControlProbe() throws Throwable {
        ClassLoader isolated =
                new ClassLoader(getClass().getClassLoader()) {
                    @Override
                    protected Class<?> loadClass(String name, boolean resolve)
                            throws ClassNotFoundException {
                        if (!name.startsWith("systems.zlink.framework.runtime.binding.")) {
                            return super.loadClass(name, resolve);
                        }
                        synchronized (getClassLoadingLock(name)) {
                            Class<?> type = findLoadedClass(name);
                            if (type == null) {
                                String resource = name.replace('.', '/') + ".class";
                                try (var input = getParent().getResourceAsStream(resource)) {
                                    if (input == null) throw new ClassNotFoundException(name);
                                    byte[] bytes = input.readAllBytes();
                                    if (name.equals(ZLinkJavaRawMeshNode.class.getName())) {
                                        ClassFile file = ClassFile.of();
                                        ClassDesc helper =
                                                ClassDesc.of(
                                                        ZLinkJavaRawMeshNodeEnvelopeProbeTest.class
                                                                .getName());
                                        bytes =
                                                file.transformClass(
                                                        file.parse(bytes),
                                                        ClassTransform.transformingMethodBodies(
                                                                method ->
                                                                        method.methodName()
                                                                                .equalsString(
                                                                                        "dispatch"),
                                                                (builder, element) -> {
                                                                    if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.owner()
                                                                                    .asInternalName()
                                                                                    .equals(
                                                                                            "systems/zlink/contracts/messaging/Message")
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "toUtf8String")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "decodePacket",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Lsystems/zlink/contracts/messaging/Message;)Ljava/lang/String;"));
                                                                    } else if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.owner()
                                                                                    .asInternalName()
                                                                                    .equals(
                                                                                            "systems/zlink/contracts/messaging/Message")
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "contentEquals")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "comparePacket",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Lsystems/zlink/contracts/messaging/Message;[B)Z"));
                                                                    } else {
                                                                        builder.with(element);
                                                                    }
                                                                }));
                                    }
                                    type = defineClass(name, bytes, 0, bytes.length);
                                } catch (IOException failure) {
                                    throw new ClassNotFoundException(name, failure);
                                }
                            }
                            if (resolve) resolveClass(type);
                            return type;
                        }
                    }
                };
        try {
            isolated.loadClass(getClass().getName()).getMethod("runScenario").invoke(null);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }

    public static String decodePacket(Message packet) {
        probes.incrementAndGet();
        textDecodes.incrementAndGet();
        decodedBytes.addAndGet(packet.size());
        return packet.toUtf8String();
    }

    public static boolean comparePacket(Message packet, byte[] expected) {
        probes.incrementAndGet();
        return packet.contentEquals(expected);
    }

    public static void runScenario() throws Exception {
        RoutingId sourceRid = RoutingId.from("envelope-probe-source");
        RoutingId targetRid = RoutingId.from("envelope-probe-target");
        AtomicInteger delivered = new AtomicInteger();
        AtomicInteger controls = new AtomicInteger();
        byte[] body = new byte[1024];
        try (var context = Zlink.createContext();
                var source = meshNode(context);
                var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://envelope-probe-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind("tcp://127.0.0.1:0");
            source.start();
            target.start();
            CompletableFuture<Void> admitted = new CompletableFuture<>();
            try (var observation =
                    source.onStateChanged(
                            () -> {
                                if (source.peers().stream()
                                        .anyMatch(peer -> peer.state() == MeshPeerState.ADMITTED)) {
                                    admitted.complete(null);
                                }
                            })) {
                source.connectPeer(target.status().localEndpoint(), targetRid);
                admitted.get(10, TimeUnit.SECONDS);
            }
            target.startDispatch(
                    record -> {
                        try (record;
                                Message packet = Message.from("reply");
                                Message reply = Message.from(body)) {
                            delivered.incrementAndGet();
                            record.reply(List.of(packet, reply));
                        }
                    });
            target.setRelocationControlHandler(
                    (sender, command) -> {
                        controls.incrementAndGet();
                        return CompletableFuture.completedFuture(command);
                    });
            var header =
                    ZLinkChannelEnvelope.create(
                            ZLinkChannelEnvelope.KIND_REQUEST,
                            "bench",
                            "Request",
                            "application/json",
                            null,
                            Map.of(),
                            null);
            try (Message packet = ZLinkChannelEnvelope.encodeHeader(header)) {
                for (int index = 0; index < 1000; index++) request(source, targetRid, packet, body);
            }
            assertEquals(1000, delivered.get());
            assertEquals(1000, probes.get(), "all requests must execute the real control probe");
            System.out.println(
                    "CONTROL_PROBE requests=1000 text_decodes="
                            + textDecodes.get()
                            + " decoded_bytes="
                            + decodedBytes.get());
            // Exercise equal-length nonmatches as well as an exact positive control.
            byte[] marker =
                    "zlink.internal.spot.relocation.control.v1".getBytes(StandardCharsets.UTF_8);
            byte[] near = marker.clone();
            near[near.length - 1] = '2';
            try (Message packet = Message.from(near)) {
                request(source, targetRid, packet, body);
            }
            near[0] = (byte) 0xff;
            try (Message packet = Message.from(near)) {
                request(source, targetRid, packet, body);
            }
            byte[] command = new byte[] {1, 2, 3};
            assertArrayEquals(
                    command,
                    source.requestRelocationControl(targetRid, command, Duration.ofSeconds(5))
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
            assertEquals(1002, delivered.get());
            assertEquals(1, controls.get());
            assertEquals(1003, probes.get());
            assertEquals(
                    0,
                    textDecodes.get(),
                    "control classification must not decode the envelope as text");
            assertEquals(0, decodedBytes.get(), "no header bytes copied for the packet probe");
        }
    }

    private static void request(
            ZLinkJavaRawMeshNode source, RoutingId target, Message packet, byte[] body)
            throws Exception {
        try (Message payload = Message.from(body)) {
            var call =
                    source.spotNode()
                            .requestToNode(
                                    target,
                                    null,
                                    List.of(packet, payload),
                                    Duration.ofSeconds(5),
                                    null,
                                    null,
                                    null);
            try (var reply = call.toCompletableFuture().get(10, TimeUnit.SECONDS)) {
                assertEquals(ZLinkBackendRequestResult.OK, reply.result());
                assertArrayEquals(body, reply.parts().get(1).toByteArray());
            }
        }
    }

    private static ZLinkJavaRawMeshNode meshNode(Context context) {
        ZLinkJavaRawMeshNode node = new ZLinkJavaRawMeshNode(context, "mesh");
        node.setApplicationJobQueue(
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.empty(),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null)));
        return node;
    }
}
