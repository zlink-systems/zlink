package systems.zlink.framework.runtime.internal.configuration;

import java.nio.ByteBuffer;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

/**
 * Test-only owner bridge packaged only in the diagnostic jar. It exercises the
 * same frozen registry selection used by the runtime without exporting an
 * internal package or introducing a production API.
 */
public final class ZLinkBenchCodecBridge {
    private ZLinkBenchCodecBridge() {
    }

    public static Session open() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.use(ZLinkProtobufCodec.defaultCodec());
        registration.freeze();
        return new Session(registration.serializerWithFallback(new ZLinkJsonMessageSerializer()));
    }

    public static final class Session {
        private final ZLinkMessageSerializer composite;

        private Session(ZLinkMessageSerializer composite) {
            this.composite = composite;
        }

        public Message encode(BenchPayload value) {
            return Message.from(ZLinkCodecRegistration.serializeForDeclaredType(
                composite, value, BenchPayload.class).bytes());
        }

        public BenchPayload decode(Message frame) {
            // This is the runtime encoded-payload boundary: materialize bytes and
            // pass them through ZLinkEncodedPayload, rather than claiming RawWire's
            // borrowed ByteBuffer behavior. Its copy cost belongs to the codec stage.
            ByteBuffer source = frame.dataBuffer().duplicate();
            byte[] bytes = new byte[source.remaining()];
            source.get(bytes);
            ZLinkMessageSerializer selected =
                ZLinkCodecRegistration.serializerForReceivedContentType(
                    composite, "application/x-protobuf");
            return selected.deserialize(ZLinkEncodedPayload.from(bytes), BenchPayload.class);
        }
    }
}
