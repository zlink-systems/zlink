package systems.zlink.stream.connector;

import java.time.Duration;

/**
 * Immutable outcome of one connector request. A successful {@link #reply()} contains an independent
 * message snapshot; the recipient owns and closes its payload when finished.
 */
public record ZLinkStreamReplyReceivedContext(
        String requestPacketName,
        String actorId,
        boolean succeeded,
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> reply,
        ZLinkStreamError error,
        Duration elapsed) {}
