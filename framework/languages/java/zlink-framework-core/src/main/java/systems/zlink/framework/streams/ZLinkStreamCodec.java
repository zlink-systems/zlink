package systems.zlink.framework.streams;

public enum ZLinkStreamCodec {
    RAW(0),
    JSON(1),
    MESSAGE_PACK(2),
    PROTOBUF(3);

    private static final ZLinkStreamCodec[] VALUES = values();

    private final int value;

    ZLinkStreamCodec(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ZLinkStreamCodec fromValue(int value) {
        for (ZLinkStreamCodec codec : VALUES) {
            if (codec.value == value) {
                return codec;
            }
        }
        throw new IllegalArgumentException("unknown stream codec: " + value);
    }
}
