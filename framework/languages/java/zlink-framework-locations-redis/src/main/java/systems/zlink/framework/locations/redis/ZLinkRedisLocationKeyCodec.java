package systems.zlink.framework.locations.redis;

import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;

/*
 * Redis row key codec. It intentionally does not call the framework runtime
 * bookkeeping codec because Redis keys are a backend storage contract shared
 * across language implementations.
 */
final class ZLinkRedisLocationKeyCodec {
    private ZLinkRedisLocationKeyCodec() {
    }

    static String encodeMeshNodeKey(ZLinkMeshNodeDescriptorKey key) {
        return encode(key.meshName(), key.rid().toHex());
    }

    static String encodeClientServerKey(
        ZLinkClientServerServerDescriptorKey key) {
        return encode(key.channelName(), key.serverRid().toHex());
    }

    static ZLinkClientServerServerDescriptorKey
        decodeClientServerKey(String encoded) {
        byte[] bytes = encoded.getBytes(StandardCharsets.UTF_8);
        Segment channel = decodeSegment(bytes, 0);
        Segment rid = decodeSegment(bytes, channel.nextOffset);
        if (rid.nextOffset != bytes.length) {
            throw new IllegalStateException(
                "invalid stored ClientServer descriptor key");
        }
        return new ZLinkClientServerServerDescriptorKey(
            channel.value,
            RoutingId.fromHex(rid.value));
    }

    static String encodeFanoutPublisherKey(
        ZLinkFanoutPublisherDescriptorKey key) {
        return encode(key.channelName(), key.publisherRid().toHex());
    }

    static ZLinkFanoutPublisherDescriptorKey
        decodeFanoutPublisherKey(String encoded) {
        byte[] bytes = encoded.getBytes(StandardCharsets.UTF_8);
        Segment channel = decodeSegment(bytes, 0);
        Segment rid = decodeSegment(bytes, channel.nextOffset);
        if (rid.nextOffset != bytes.length) {
            throw new IllegalStateException(
                "invalid stored fanout publisher descriptor key");
        }
        return new ZLinkFanoutPublisherDescriptorKey(
            channel.value,
            RoutingId.fromHex(rid.value));
    }

    static ZLinkMeshNodeDescriptorKey decodeMeshNodeKey(String encoded) {
        byte[] bytes = encoded.getBytes(StandardCharsets.UTF_8);
        Segment mesh = decodeSegment(bytes, 0);
        Segment rid = decodeSegment(bytes, mesh.nextOffset);
        if (rid.nextOffset != bytes.length) {
            throw new IllegalStateException(
                "invalid stored MeshNode descriptor key");
        }
        return new ZLinkMeshNodeDescriptorKey(
            mesh.value,
            RoutingId.fromHex(rid.value));
    }

    private static String encode(String... segments) {
        StringBuilder builder = new StringBuilder();
        for (String segment : segments) {
            String safe = nullToEmpty(segment);
            builder.append(
                    safe.getBytes(StandardCharsets.UTF_8).length)
                .append(':')
                .append(safe);
        }
        return builder.toString();
    }

    private static Segment decodeSegment(byte[] bytes, int offset) {
        int colon = offset;
        while (colon < bytes.length && bytes[colon] != ':') {
            if (bytes[colon] < '0' || bytes[colon] > '9') {
                throw new IllegalStateException(
                    "invalid stored MeshNode descriptor key");
            }
            colon++;
        }
        if (colon == offset || colon == bytes.length) {
            throw new IllegalStateException(
                "invalid stored MeshNode descriptor key");
        }
        int length;
        try {
            length = Integer.parseInt(new String(
                bytes,
                offset,
                colon - offset,
                StandardCharsets.US_ASCII));
        } catch (NumberFormatException error) {
            throw new IllegalStateException(
                "invalid stored MeshNode descriptor key",
                error);
        }
        int start = colon + 1;
        int end = start + length;
        if (length < 0 || end < start || end > bytes.length) {
            throw new IllegalStateException(
                "invalid stored MeshNode descriptor key");
        }
        return new Segment(
            new String(bytes, start, length, StandardCharsets.UTF_8),
            end);
    }

    private record Segment(String value, int nextOffset) {
    }

    private static String nullToEmpty(String value) {
        return value == null ? "" : value;
    }

}
