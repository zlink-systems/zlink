package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAuthorityPut(
    byte[] payload)
    implements ZLinkAuthorityMutation {
    public ZLinkAuthorityPut {
        payload = Objects.requireNonNull(payload, "payload").clone();
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
