package systems.zlink.framework.runtime.internal.relocation;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** The host stop deadline elapsed while the relocation unit's authority was unsettled. */
public final class ZLinkAuthorityUnsettledAtStopException extends ZLinkFrameworkException {
    public ZLinkAuthorityUnsettledAtStopException() {
        super(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "Relocation unit is unsettled at the host stop deadline; retry on another node");
    }
}
