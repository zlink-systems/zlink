package systems.zlink.framework.runtime.locations;

import java.util.HexFormat;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;

/*
 * Framework-internal row key codec for in-memory bookkeeping and runtime
 * generation tracking. Backend extensions own their transport key codec, so
 * Redis keys are encoded in zlink-framework-locations-redis instead of calling
 * through this type.
 */
final class ZLinkLocationKeyCodec {
    private static final HexFormat HEX = HexFormat.of();

    private ZLinkLocationKeyCodec() {
    }

    static String encodeFanoutPublisherKey(
        ZLinkFanoutPublisherDescriptorKey key) {
        return encode(key.channelName(), toHex(key.publisherRid()));
    }

    private static String encode(String... segments) {
        StringBuilder builder = new StringBuilder();
        for (String segment : segments) {
            String safeSegment = nullToEmpty(segment);
            builder.append(safeSegment.length()).append(':').append(safeSegment);
        }
        return builder.toString();
    }

    private static String nullToEmpty(String value) {
        return value == null ? "" : value;
    }

    private static String toHex(RoutingId routingId) {
        return HEX.formatHex(routingId.toBytes());
    }

}
