package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAuthorityScanCursor(String encoded) {
    public ZLinkAuthorityScanCursor {
        Objects.requireNonNull(encoded, "encoded");
    }
}
