/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.google.protobuf.ByteString;
import com.google.protobuf.CodedOutputStream;
import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.UnsafeByteOperations;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.contracts.messaging.Message;

/**
 * Wire shape of the {@code zlink-<lang>} raw row.
 *
 * <p>The raw row is measured against {@code zlink-c} (spec section 7.2 formula 1), so it
 * must put the same bytes on the wire as {@code framework/bench/grpc/c}. That
 * bench sends a two-part message: an envelope header part and a protobuf-encoded
 * BenchPayload part (bench_zlink_client.cpp:14-16 and :130-140). The .NET and node
 * raw rows do the same. Sending a bare payload here would make formula 1 divide
 * two different experiments (FB-024).
 */
public final class RawWire {
    public static final byte[] REQUEST_ENVELOPE = (
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\","
        + "\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,"
        + "\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}")
        .getBytes(StandardCharsets.UTF_8);

    public static final byte[] RESPONSE_ENVELOPE = (
        "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\","
        + "\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,"
        + "\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}")
        .getBytes(StandardCharsets.UTF_8);

    public static final String RAW_REQUEST_SERVER_ID = "bench-raw-request-server";
    public static final String RAW_COMMAND_SERVER_ID = "bench-raw-command-server";

    private RawWire() {
    }

    /** Creates and serializes a typed payload for every raw send/request. */
    public static Message encodeBenchPayloadMessage(
        int payloadSize, int runId, byte phase, long sequence) {
        return encodeBenchPayloadMessage(UnsafeByteOperations.unsafeWrap(
            BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence)));
    }

    /** Replies preserve the received measurement header, including its timestamp. */
    public static Message encodeBenchPayloadMessage(ByteBuffer body) {
        return encodeBenchPayloadMessage(UnsafeByteOperations.unsafeWrap(body.duplicate()));
    }

    private static Message encodeBenchPayloadMessage(ByteString body) {
        BenchPayload payload = BenchPayload.newBuilder()
            .setBody(body)
            .build();
        Message message = Message.allocate(payload.getSerializedSize());
        try {
            CodedOutputStream output = CodedOutputStream.newInstance(message.mutableDataBuffer());
            payload.writeTo(output);
            output.flush();
            return message;
        } catch (IOException error) {
            message.close();
            throw new IllegalStateException("cannot serialize BenchPayload", error);
        }
    }

    /** Parses BenchPayload with the same protobuf runtime used by the typed stacks. */
    public static ByteBuffer decodeBenchPayloadBody(ByteBuffer encoded) {
        if (encoded == null) {
            return null;
        }
        try {
            return BenchPayload.parseFrom(encoded.duplicate()).getBody().asReadOnlyByteBuffer();
        } catch (InvalidProtocolBufferException error) {
            return null;
        }
    }
}
