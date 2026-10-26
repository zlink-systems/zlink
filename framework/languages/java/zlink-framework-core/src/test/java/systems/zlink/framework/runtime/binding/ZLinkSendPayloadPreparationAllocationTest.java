package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.Optional;
import java.util.concurrent.CompletionException;

/** Public send preparation consumes codec output without an intermediate payload tuple. */
public final class ZLinkSendPayloadPreparationAllocationTest {
    private static volatile Object observed;
    private static final int SENDS = 100;
    private static final byte[] PAYLOAD = new byte[1024];
    private static final String CONTENT_TYPE = "application/json";
    private static final Object VALUE = new Object();
    private static final RoutingId TARGET = RoutingId.from("send-prepare-target");

    @Test
    void publicSendPreparationDoesNotWrapOwnedCodecOutput() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    private static final class Serializer
            implements ZLinkMessageSerializer, ZLinkOwnedMessageSerializer {
        private Message output;

        @Override
        public Message serializeOwned(Object value) {
            return output = Message.from(PAYLOAD);
        }

        @Override
        public <T> T deserializeBorrowed(ByteBuffer value, Class<T> type) {
            throw new AssertionError();
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new AssertionError("owned serializer must be used");
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload value, Class<T> type) {
            throw new AssertionError();
        }
    }

    public static void main(String[] args) throws Exception {
        var serializer = new Serializer();
        var options = new DefaultZLinkFrameworkOptions();
        try (var runtime =
                new ZLinkChannelRuntime(
                        new ZLinkJavaChannelBackendAdapter(), options.registration(), serializer)) {
            long total = 0;
            long totalOptionals = 0;
            long excessCallbacks = 0;
            for (boolean direct : new boolean[] {false, true}) {
                var calls = new ZLinkSendCall[SENDS];
                var outputs = new Message[SENDS];
                for (int i = 0; i < 200; i++) {
                    reject(prepare(runtime, direct));
                    assertThrows(IllegalStateException.class, serializer.output::refCount);
                }
                long[] control =
                        tuples(
                                () -> {
                                    for (int i = 0; i < SENDS; i++) {
                                        var encoded =
                                                ZLinkPayloadEncoding.encode(
                                                        serializer, VALUE, CONTENT_TYPE);
                                        encoded.payload().close();
                                    }
                                });
                long[] optionalControl =
                        tuples(
                                () -> {
                                    for (int i = 0; i < SENDS; i++) observed = Optional.of(VALUE);
                                });
                long[] actual;
                try {
                    actual =
                            tuples(
                                    () -> {
                                        for (int i = 0; i < SENDS; i++) {
                                            calls[i] = prepare(runtime, direct);
                                            outputs[i] = serializer.output;
                                        }
                                    });
                } finally {
                    for (int i = 0; i < SENDS; i++)
                        if (calls[i] != null) {
                            reject(calls[i]);
                            assertThrows(IllegalStateException.class, outputs[i]::refCount);
                        }
                }
                System.out.printf(
                        "direct=%s sends=%d controlTuples=%d prepareTuples=%d controlOptionals=%d prepareOptionals=%d%n",
                        direct, SENDS, control[0], actual[0], optionalControl[1], actual[1]);
                assertEquals(
                        SENDS,
                        control[0],
                        "the explicit composite codec result is the positive control");
                assertEquals(
                        SENDS,
                        optionalControl[1],
                        "Optional allocation recording has a positive control");
                long[] callbackControl =
                        tuples(
                                () -> {
                                    for (int i = 0; i < SENDS; i++)
                                        observed = (Runnable) () -> serializer.output.close();
                                },
                                true);
                long[] submitted =
                        tuples(
                                () -> {
                                    for (int i = 0; i < SENDS; i++) {
                                        reject(prepare(runtime, direct));
                                        assertThrows(
                                                IllegalStateException.class,
                                                serializer.output::refCount);
                                    }
                                });
                System.out.printf(
                        "direct=%s sends=%d controlCallbacks=%d submitCallbacks=%d%n",
                        direct, SENDS, callbackControl[2], submitted[2]);
                assertEquals(SENDS, callbackControl[2]);
                excessCallbacks += Math.abs(submitted[2] - callbackControl[2]);
                total += actual[0];
                totalOptionals += actual[1];
            }
            assertEquals(0, total, "the send calls directly own the codec output");
            assertEquals(0, totalOptionals, "send preparation keeps packet names as strings");
            assertEquals(
                    0,
                    excessCallbacks,
                    "the send call supplies its own mesh submit and rejection callback");
        } finally {
            options.registration().applicationJobQueue().close();
        }
    }

    private static ZLinkSendCall prepare(ZLinkChannelRuntime runtime, boolean direct) {
        return direct
                ? runtime.sendToNode("unregistered", TARGET, VALUE)
                : runtime.sendToChannel("unregistered", VALUE);
    }

    private static void reject(ZLinkSendCall call) {
        var failure =
                assertThrows(
                        CompletionException.class,
                        () -> call.submit().toCompletableFuture().join());
        assertInstanceOf(
                systems.zlink.framework.errors.ZLinkConfigurationException.class,
                failure.getCause());
    }

    private static long[] tuples(Runnable action) throws Exception {
        return tuples(action, false);
    }

    private static long[] tuples(Runnable action, boolean callbackControl) throws Exception {
        var file = Files.createTempFile("zlink-send-prepare-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                action.run();
                recording.stop();
                recording.dump(file);
            }
            long[] count = new long[3];
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")) {
                        String allocated = event.getClass("objectClass").getName();
                        if (allocated.equals(ZLinkPayloadEncoding.EncodedPayload.class.getName()))
                            count[0]++;
                        if (allocated.equals(Optional.class.getName())) count[1]++;
                        if (allocated.contains("$$Lambda") && event.getStackTrace() != null) {
                            var owner =
                                    event.getStackTrace().getFrames().stream()
                                            .filter(
                                                    frame ->
                                                            frame.getMethod()
                                                                    .getType()
                                                                    .getName()
                                                                    .startsWith(
                                                                            "systems.zlink.framework."))
                                            .findFirst();
                            if (owner.isPresent()) {
                                String type = owner.orElseThrow().getMethod().getType().getName();
                                if (callbackControl
                                        ? type.startsWith(
                                                ZLinkSendPayloadPreparationAllocationTest.class
                                                        .getName())
                                        : type.startsWith(
                                                        "systems.zlink.framework.runtime.channels.RouteSendCall")
                                                || type.startsWith(
                                                        "systems.zlink.framework.runtime.channels.ChannelSendCall"))
                                    count[2]++;
                            }
                        }
                    }
                }
            }
            return count;
        } finally {
            Files.deleteIfExists(file);
        }
    }
}
