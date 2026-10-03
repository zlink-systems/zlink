package systems.zlink.stream.connector;

import java.util.logging.Level;
import java.util.logging.Logger;

final class ZLinkStreamCleanup {
    private static final Logger LOGGER = Logger.getLogger(ZLinkStreamCleanup.class.getName());

    private ZLinkStreamCleanup() {}

    static void close(AutoCloseable resource) {
        if (resource == null) return;
        try {
            resource.close();
        } catch (Exception failure) {
            LOGGER.log(Level.WARNING, "STREAM cleanup failed", failure);
        }
    }

    static void closeMessage(ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        close(message.payload().payload());
    }
}
