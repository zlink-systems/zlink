package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.testsupport.JfrAllocations;

import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/**
 * Counts, per item, the heap objects the Java binding allocates on its send paths: the rotation
 * permit storage (I04) and the STREAM heartbeat frame (T03). Each fixture runs in a child JVM with
 * TLAB disabled so every allocation is one exact event.
 */
public final class ZLinkJavaSendPathHeaderAllocationTest {
    private static final int SAMPLES = 100;

    @Test
    void receiveRotationKeepsNoSeparatePermitStorage() throws Exception {
        JfrAllocations.runInChild(getClass(), "permit-storage");
    }

    @Test
    void heartbeatFramesAreNotEncodedPerSend() throws Exception {
        JfrAllocations.runInChild(getClass(), "heartbeat");
    }

    @Test
    void heartbeatFrameBytesEqualTheDynamicEncoding() {
        for (String name : List.of("$zlink.heartbeat.ping", "$zlink.heartbeat.pong")) {
            byte[] expected =
                    ZLinkStreamFrameCodec.encode(
                            ZLinkStreamHeaderCodec.encode(dynamicHeader(name)), new byte[0]);
            try (Message body = Message.from(new byte[0]);
                    Message frame = ZLinkJavaStreamFraming.frame(heartbeat(name), List.of(body))) {
                assertArrayEquals(expected, frame.toByteArray());
            }
        }
    }

    public static void main(String[] args) throws Exception {
        switch (args[0]) {
            case "permit-storage" -> permitStorage();
            case "heartbeat" -> heartbeatFrames();
            default -> throw new IllegalArgumentException(args[0]);
        }
    }

    private static void permitStorage() throws Exception {
        try (Context context = Zlink.createContext()) {
            var created = new ArrayList<ZLinkJavaRawMeshNode>();
            var events =
                    JfrAllocations.record(
                            () -> {
                                for (int index = 0; index < 10; index++) {
                                    created.add(
                                            new ZLinkJavaRawMeshNode(context, "permit-storage"));
                                }
                            });
            long permitArrays =
                    JfrAllocations.count(
                            events,
                            event ->
                                    event.type().startsWith("[L")
                                            && event.type()
                                                    .contains("ZLinkApplicationJobQueue$Permit")
                                            && event.passesThroughType(
                                                    ZLinkJavaRawMeshNode.class.getName()));
            long nodes =
                    JfrAllocations.count(
                            events,
                            event -> event.type().equals(ZLinkJavaRawMeshNode.class.getName()));
            System.out.printf("nodes=%d permitArrays=%d%n", nodes, permitArrays);
            assertEquals(10, nodes, "the nodes were constructed inside the recording (control)");
            assertEquals(0, permitArrays, "a claimed record owns its permit");
            for (var node : created) node.close();
        }
    }

    private static void heartbeatFrames() throws Exception {
        for (String name : List.of("$zlink.heartbeat.ping", "$zlink.heartbeat.pong")) {
            ZLinkStreamHeader header = heartbeat(name);
            for (int index = 0; index < 50; index++) {
                try (Message body = Message.from(new byte[0]);
                        Message frame = ZLinkJavaStreamFraming.frame(header, List.of(body))) {
                    assertTrue(frame.size() > 0);
                }
            }
            var events =
                    JfrAllocations.record(
                            () -> {
                                for (int index = 0; index < SAMPLES; index++) {
                                    try (Message body = Message.from(new byte[0]);
                                            Message frame =
                                                    ZLinkJavaStreamFraming.frame(
                                                            header, List.of(body))) {
                                        assertTrue(frame.size() > 0);
                                    }
                                }
                            });
            long encodedArrays =
                    JfrAllocations.count(
                            events,
                            event ->
                                    event.type().equals("[B")
                                            && event.passesThrough(
                                                    ZLinkJavaStreamFraming.class.getName(), "frame")
                                            && event.frameworkFrame()
                                                    .startsWith(
                                                            "systems.zlink.framework.runtime.streams."));
            System.out.printf("%s frames=%d codecArrays=%d%n", name, SAMPLES, encodedArrays);
            assertEquals(0, encodedArrays, "a heartbeat frame is encoded once, not per send");
        }
    }

    private static ZLinkStreamHeader heartbeat(String name) {
        return name.endsWith("ping")
                ? ZLinkStreamHeader.HEARTBEAT_PING
                : ZLinkStreamHeader.HEARTBEAT_PONG;
    }

    private static ZLinkStreamHeader dynamicHeader(String name) {
        return new ZLinkStreamHeader(
                ZLinkStreamMessageKind.CONTROL,
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                name,
                Map.of(),
                Optional.empty());
    }
}
