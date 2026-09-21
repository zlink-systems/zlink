package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locationprovider.ZLinkStoreKey;

import java.util.Objects;

final class ZLinkOpaqueRecordKey {
    private ZLinkOpaqueRecordKey() {}

    static ZLinkStoreKey of(String record, String... segments) {
        StringBuilder preimage = new StringBuilder(requireSegment(record, "record"));
        for (int index = 0; index < segments.length; index++) {
            preimage.append('\0')
                    .append(requireSegment(segments[index], "segments[" + index + "]"));
        }
        return new ZLinkStoreKey(preimage.toString());
    }

    private static String requireSegment(String value, String field) {
        Objects.requireNonNull(value, field);
        if (value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(field + " must not contain NUL");
        }
        return value;
    }
}
