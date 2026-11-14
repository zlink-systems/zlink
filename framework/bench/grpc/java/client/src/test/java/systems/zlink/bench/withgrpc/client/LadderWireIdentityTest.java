/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.BenchContract;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.BenchTypedPayload;
import systems.zlink.bench.withgrpc.shared.LadderProtobuf;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

/** Byte comparison, with the pre-change encoding/read/rebuild path retained as the oracle. */
public final class LadderWireIdentityTest {
    private static final int RUN_ID = 0x01020304;
    private static final byte PHASE = BenchMetricHeader.PHASE_ACTIVE;
    private static final long SEQUENCE = 1234;
    private static final long CORRELATION = SEQUENCE + 1;
    private static final ZLinkServiceM6AWireCodec WIRE = new ZLinkServiceM6AWireCodec();
    private static final ZLinkJsonMessageSerializer JSON = new ZLinkJsonMessageSerializer();
    private static final ZLinkChannelEnvelope.PreparedNames CHANNEL =
            new ZLinkChannelEnvelope.PreparedNames(BenchContract.CHANNEL_NAME, null, null);

    public static void main(String[] args) throws Exception {
        WIRE.prepareChannelNames(List.of(BenchContract.CHANNEL_NAME));
        int frames = 0;
        for (int level : new int[] {2, 3, 4}) {
            for (int size : new int[] {1024, 4096}) {
                boolean json = level == 4;
                // Invoke the actual L3/L4 source helper. L2 uses the same protobuf encoder.
                try (var afterBody = sourceBody(size, json)) {
                    byte[] metricBody = metricBody(afterBody, json);
                    try (var beforeBody = beforeBody(metricBody, json)) {
                        for (int kind : new int[] {
                                ZLinkChannelEnvelope.KIND_REQUEST,
                                ZLinkChannelEnvelope.KIND_COMMAND}) {
                            var header = header(kind, json);
                            try (var beforeEnvelope = envelope(header, cloneBody(beforeBody));
                                 var afterEnvelope = envelope(header, cloneBody(afterBody))) {
                                compare(wire(level, kind, beforeEnvelope),
                                        wire(level, kind, afterEnvelope), label(level, size, kind));
                                frames++;
                                if (kind == ZLinkChannelEnvelope.KIND_REQUEST) {
                                    compare(reply(level, beforeEnvelope, true),
                                            reply(level, afterEnvelope, false),
                                            label(level, size, ZLinkChannelEnvelope.KIND_RESPONSE));
                                    frames++;
                                    verifyCompletion(level, size, afterEnvelope);
                                }
                            }
                        }
                    }
                }
            }
        }
        // Prove that the comparator rejects changed payload bytes and missing frames.
        expectMismatch(new byte[][] {{1, 2}}, new byte[][] {{1, 3}});
        expectMismatch(new byte[][] {{1}, {2}}, new byte[][] {{1}});
        System.out.println("JAVA_LADDER_WIRE_IDENTITY_OK frames=" + frames
                + " negative_controls=2 baseline=52c1761ab0");
    }

    private static ZLinkFrameworkMultipartView sourceBody(int size, boolean json) throws Exception {
        Method method = RawStack.class.getDeclaredMethod("payloadView",
                int.class, int.class, byte.class, long.class, boolean.class);
        method.setAccessible(true);
        return (ZLinkFrameworkMultipartView) method.invoke(
                null, size, RUN_ID, PHASE, SEQUENCE, json);
    }

    private static byte[] metricBody(List<Message> encoded, boolean json) {
        var buffer = ZLinkFrameworkMultipartView.buffer(encoded, 0);
        return json ? ZLinkMessagePayloads.deserialize(JSON, buffer, BenchTypedPayload.class).body()
                : parseBenchPayload(buffer).getBody().toByteArray();
    }

    private static BenchPayload parseBenchPayload(ByteBuffer encoded) {
        try {
            return BenchPayload.parseFrom(encoded);
        } catch (com.google.protobuf.InvalidProtocolBufferException failure) {
            throw new AssertionError(failure);
        }
    }

    // 52c1761ab0 RW:49-53 is deliberately unchanged: copyFrom + toByteArray + Message.from.
    private static ZLinkFrameworkMultipartView beforeBody(byte[] body, boolean json) {
        return json ? ZLinkPayloadEncoding.encodeMessage(
                JSON, new BenchTypedPayload(body), "application/json")
                : ZLinkFrameworkMultipartView.owned(
                        RawWire.encodeBenchPayloadMessage(ByteBuffer.wrap(body)));
    }

    private static ZLinkFrameworkMultipartView cloneBody(List<Message> body) {
        return ZLinkFrameworkMultipartView.owned(
                Message.from(ZLinkFrameworkMultipartView.buffer(body, 0)));
    }

    private static ZLinkChannelEnvelope.Header header(int kind, boolean json) {
        return ZLinkChannelEnvelope.create(kind, BenchContract.CHANNEL_NAME,
                json ? "BenchTypedPayload" : "BenchPayload",
                json ? "application/json" : "application/x-protobuf", null, Map.of(), null,
                UUID.fromString("018f1234-5678-7000-8000-000000000001"));
    }

    private static ZLinkFrameworkMultipartView envelope(
            ZLinkChannelEnvelope.Header header, ZLinkFrameworkMultipartView body) {
        var names = new ZLinkChannelEnvelope.PreparedNames(
                null, header.messageName(), header.contentType());
        return (ZLinkFrameworkMultipartView) ZLinkChannelEnvelope.encode(
                header, body, CHANNEL, names, names);
    }

    private static byte[][] wire(int level, int kind, List<Message> envelope) {
        if (level == 2) {
            return new byte[][] {bytes(ZLinkFrameworkMultipartView.buffer(envelope, 0)),
                    bytes(ZLinkFrameworkMultipartView.buffer(envelope, 1))};
        }
        byte[] serviceHeader = switch (kind) {
            case ZLinkChannelEnvelope.KIND_REQUEST ->
                    WIRE.encodeChannelRequestHeader(CORRELATION, BenchContract.CHANNEL_NAME, 0);
            case ZLinkChannelEnvelope.KIND_COMMAND ->
                    WIRE.encodeChannelSendHeader(BenchContract.CHANNEL_NAME, 0);
            case ZLinkChannelEnvelope.KIND_RESPONSE -> WIRE.encodeReplyHeader(CORRELATION, 0, 0);
            default -> throw new AssertionError("unexpected kind");
        };
        try (Message framed = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(envelope)) {
            return new byte[][] {serviceHeader, framed.toByteArray()};
        }
    }

    private static byte[][] reply(int level, List<Message> request, boolean before) {
        try (Message packed = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(request);
             var view = ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView(packed.dataBuffer())) {
            // 52c1761ab0 T:308,321,387,398 materialized the view on both reads.
            var header = ZLinkChannelEnvelope.decodeHeader(before
                    ? view.getFirst().dataBuffer() : ZLinkFrameworkMultipartView.buffer(view, 0), false);
            ByteBuffer body = before ? view.getLast().dataBuffer()
                    : ZLinkFrameworkMultipartView.buffer(view, 1);
            ZLinkFrameworkMultipartView encoded;
            if (level == 4) {
                var payload = ZLinkMessagePayloads.deserialize(JSON, body, BenchTypedPayload.class);
                encoded = ZLinkPayloadEncoding.encodeMessage(JSON, payload, "application/json");
            } else if (before) {
                // 52c1761ab0 T:255-256,398-406 parsed the body and rebuilt a new protobuf.
                encoded = ZLinkFrameworkMultipartView.owned(
                        RawWire.encodeBenchPayloadMessage(RawWire.decodeBenchPayloadBody(body)));
            } else {
                encoded = LadderProtobuf.encode(LadderProtobuf.decode(body));
            }
            try (var response = envelope(ZLinkChannelEnvelope.reply(header), encoded)) {
                return wire(level, ZLinkChannelEnvelope.KIND_RESPONSE, response);
            }
        }
    }

    private static void verifyCompletion(int level, int size, List<Message> request) throws Exception {
        byte[][] reply = reply(level, request, false);
        if (level == 2) return;
        try (Message packed = Message.from(reply[1]);
             var view = ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView(packed.dataBuffer())) {
            Method validator = RawStack.class.getDeclaredMethod(
                    level == 4 ? "validateTypedEnvelope" : "validateEnvelopeParts",
                    List.class, int.class, byte.class, int.class, long.class);
            validator.setAccessible(true);
            validator.invoke(null, view, RUN_ID, PHASE, size, SEQUENCE);
            Field slots = ZLinkFrameworkMultipartView.class.getDeclaredField("parts");
            slots.setAccessible(true);
            for (Object part : (Object[]) slots.get(view)) {
                if (part instanceof Message) {
                    throw new AssertionError("completion materialized a native Message at L" + level);
                }
            }
        }
    }

    private static byte[] bytes(ByteBuffer buffer) {
        byte[] result = new byte[buffer.remaining()];
        buffer.get(result);
        return result;
    }

    private static String label(int level, int size, int kind) {
        return "L" + level + " size=" + size + " kind=" + kind;
    }

    private static void compare(byte[][] before, byte[][] after, String label) {
        if (!Arrays.deepEquals(before, after)) throw new AssertionError("wire mismatch " + label);
        System.out.println("WIRE_IDENTICAL " + label + " parts=" + before.length);
    }

    private static void expectMismatch(byte[][] before, byte[][] after) {
        try {
            compare(before, after, "negative control");
        } catch (AssertionError expected) {
            return;
        }
        throw new AssertionError("wire comparator accepted a mismatch");
    }
}
