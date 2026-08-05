/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;


import java.nio.charset.StandardCharsets;

/**
 * Snapshot entry returned from SUB/XSUB/XPUB subscription snapshots.
 * @param filter the subscription filter string
 * @param pattern whether the filter is a pattern match
 */
public record SubscriptionEntry(String filter, boolean pattern) {
    public SubscriptionEntry {
        filter = filter == null ? "" : filter;
    }

    public byte[] filterBytes() {
        return filter.getBytes(StandardCharsets.UTF_8);
    }

    public static SubscriptionEntry fromBytes(byte[] filter, boolean pattern) {
        return new SubscriptionEntry(filter == null ? ""
            : new String(filter, StandardCharsets.UTF_8), pattern);
    }

    static SubscriptionEntry fromFilter(String filter, boolean pattern) {
        return new SubscriptionEntry(filter, pattern);
    }
}
