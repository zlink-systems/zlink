package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkAuthorityMutation
        permits ZLinkAuthorityPut,
                ZLinkAuthorityRestore,
                ZLinkAuthorityDelete,
                ZLinkAuthorityReincarnate {
    static ZLinkAuthorityMutation reincarnate(byte[] payload) {
        return new ZLinkAuthorityReincarnate(payload);
    }

    static byte[] reincarnatePayload(ZLinkAuthorityMutation mutation) {
        return mutation instanceof ZLinkAuthorityReincarnate reincarnate
                ? reincarnate.payload()
                : null;
    }
}

record ZLinkAuthorityReincarnate(byte[] payload) implements ZLinkAuthorityMutation {
    ZLinkAuthorityReincarnate {
        payload = java.util.Objects.requireNonNull(payload, "payload").clone();
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
