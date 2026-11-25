package systems.zlink.stream.connector;

import java.time.Duration;

/** An authentication admission rejection received for a pending request. */
public record ZLinkStreamAdmissionWaiting(
        String requestPacketName, String actorId, Duration retryAfter) {}
