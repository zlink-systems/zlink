package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

import java.util.Objects;

public record ZLinkAuthorityConflict(
        ZLinkAuthorityReadResult current, ZLinkFrameworkErrorKind membershipFailure)
        implements ZLinkAuthorityWriteResult {
    public ZLinkAuthorityConflict(ZLinkAuthorityReadResult current) {
        this(current, null);
    }

    public ZLinkAuthorityConflict {
        Objects.requireNonNull(current, "current");
    }
}
