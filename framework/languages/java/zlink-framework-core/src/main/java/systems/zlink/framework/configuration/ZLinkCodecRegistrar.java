package systems.zlink.framework.configuration;

import java.util.function.Predicate;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

public interface ZLinkCodecRegistrar {
    /**
     * Registers a payload serializer under a content type. Codec extensions use
     * this registrar so application configuration can stay limited to
     * {@link ZLinkCodecRegistryBuilder#use(ZLinkCodecExtension)}.
     */
    void addSerializer(String contentType, ZLinkMessageSerializer serializer);

    /**
     * Registers a payload serializer that is used only when {@code canSerialize}
     * returns {@code true} for the declared payload type.
     */
    void addSerializer(
        String contentType,
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize);

    /**
     * Maps a registered serializer content type to the stream packet codec value
     * used on the wire.
     */
    void addStreamCodec(String contentType, ZLinkStreamCodec codec);
}
