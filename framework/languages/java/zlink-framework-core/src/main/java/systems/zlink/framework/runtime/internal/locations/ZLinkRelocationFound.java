package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkRelocationFound(byte[] payload)
    implements ZLinkRelocationReadResult {
    public ZLinkRelocationFound {
        payload = Objects.requireNonNull(payload, "payload").clone();
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
