package systems.zlink.stream.connector;

@FunctionalInterface
public interface ZLinkStreamPacketNameResolver {
    String resolve(Class<?> payloadType);

    static ZLinkStreamPacketNameResolver defaultResolver() {
        return payloadType -> {
            ZLinkStreamPacketName packetName =
                payloadType.getAnnotation(ZLinkStreamPacketName.class);
            if (packetName != null) {
                return packetName.value();
            }
            return payloadType.getSimpleName();
        };
    }
}
