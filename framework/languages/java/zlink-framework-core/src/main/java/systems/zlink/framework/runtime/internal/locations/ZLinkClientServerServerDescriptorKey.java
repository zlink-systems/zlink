package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.util.Objects;

public record ZLinkClientServerServerDescriptorKey(String channelName, RoutingId serverRid) {
    public ZLinkClientServerServerDescriptorKey {
        if (channelName == null || channelName.isBlank() || channelName.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("channelName must be non-blank text without NUL");
        }
        Objects.requireNonNull(serverRid, "serverRid");
    }
}
