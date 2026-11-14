/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.google.protobuf.UnsafeByteOperations;
import java.nio.ByteBuffer;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

/** L2/L3 use the production protobuf serializer; RawWire remains the raw binding baseline. */
public final class LadderProtobuf {
    private static final String CONTENT_TYPE = "application/x-protobuf";
    private static final ZLinkMessageSerializer SERIALIZER = serializer();

    private LadderProtobuf() {}

    private static ZLinkMessageSerializer serializer() {
        var codecs = new ZLinkCodecRegistration();
        codecs.use(ZLinkProtobufCodec.defaultCodec());
        var serializer = codecs.serializers().get(CONTENT_TYPE);
        serializer.prepare(BenchPayload.class);
        return serializer;
    }

    public static ZLinkFrameworkMultipartView encode(
            int payloadSize, int runId, byte phase, long sequence) {
        var payload = BenchPayload.newBuilder()
                .setBody(UnsafeByteOperations.unsafeWrap(
                        BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence)))
                .build();
        return encode(payload);
    }

    /** Echo the parsed request, including protobuf unknown fields, as production does. */
    public static ZLinkFrameworkMultipartView encode(BenchPayload payload) {
        return ZLinkPayloadEncoding.encodeMessage(SERIALIZER, payload, CONTENT_TYPE);
    }

    public static BenchPayload decode(ByteBuffer encoded) {
        return ZLinkMessagePayloads.deserialize(SERIALIZER, encoded, BenchPayload.class);
    }
}
