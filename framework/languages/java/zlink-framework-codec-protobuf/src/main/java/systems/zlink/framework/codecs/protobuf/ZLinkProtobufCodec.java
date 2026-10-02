package systems.zlink.framework.codecs.protobuf;

import systems.zlink.framework.configuration.ZLinkCodecExtension;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamTypedCodec;

public final class ZLinkProtobufCodec implements ZLinkCodecExtension, ZLinkStreamTypedCodec {
    private static final String CONTENT_TYPE = "application/x-protobuf";
    private static final ZLinkProtobufCodec DEFAULT = new ZLinkProtobufCodec();

    private ZLinkProtobufCodec() {}

    public static ZLinkProtobufCodec defaultCodec() {
        return DEFAULT;
    }

    @Override
    public <T> ZLinkStreamEncodedPayload encode(String packetName, T value) {
        return ZLinkProtobufStreamCodec.INSTANCE.encode(packetName, value);
    }

    @Override
    public <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type) {
        return ZLinkProtobufStreamCodec.INSTANCE.decode(payload, type);
    }

    @Override
    public void register(ZLinkCodecRegistrar codecs) {
        codecs.addSerializer(
                CONTENT_TYPE,
                ZLinkProtobufMessageSerializer.INSTANCE,
                ZLinkProtobufMessageSerializer::canSerialize);
        codecs.addStreamCodec(CONTENT_TYPE, ZLinkStreamCodec.PROTOBUF);
    }
}
