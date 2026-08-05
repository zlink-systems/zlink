package systems.zlink.framework.codecs.msgpack;

import java.util.Objects;
import java.util.function.Predicate;
import systems.zlink.framework.configuration.ZLinkCodecExtension;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamTypedCodec;

public final class ZLinkMessagePackCodec implements ZLinkCodecExtension, ZLinkStreamTypedCodec {
    private static final ZLinkMessagePackCodec DEFAULT =
        new ZLinkMessagePackCodec(ignored -> true);
    private final Predicate<Class<?>> canSerialize;

    private ZLinkMessagePackCodec(Predicate<Class<?>> canSerialize) {
        this.canSerialize = Objects.requireNonNull(canSerialize, "canSerialize");
    }

    public static ZLinkMessagePackCodec defaultCodec() {
        return DEFAULT;
    }

    public static ZLinkMessagePackCodec forPayloadTypes(Predicate<Class<?>> canSerialize) {
        return new ZLinkMessagePackCodec(canSerialize);
    }

    @Override
    public <T> ZLinkStreamEncodedPayload encode(String packetName, T value) {
        return ZLinkMessagePackStreamCodec.INSTANCE.encode(packetName, value);
    }

    @Override
    public <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type) {
        return ZLinkMessagePackStreamCodec.INSTANCE.decode(payload, type);
    }

    @Override
    public void register(ZLinkCodecRegistrar codecs) {
        codecs.addSerializer(
            "application/x-msgpack",
            ZLinkMessagePackMessageSerializer.INSTANCE,
            canSerialize);
        codecs.addStreamCodec("application/x-msgpack", ZLinkStreamCodec.MESSAGE_PACK);
    }
}
