package systems.zlink.framework.runtime.streams;

/** Internal marker for the STREAM EMSGSIZE terminal path. */
final class ZLinkStreamMessageTooLargeException extends IllegalArgumentException {
    static final int EMSGSIZE = 90;

    ZLinkStreamMessageTooLargeException(String message) {
        super(message);
    }
}
