package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * A Location Store/provider, owner-lease or conditional-write failure. These boundaries preserve
 * the Store origin for host relocation spec 30 §13 {@code StoreUnavailable}.
 */
public final class ZLinkLocationStoreFailureException extends ZLinkFrameworkException {
    ZLinkLocationStoreFailureException(Throwable cause) {
        this("Location Store provider is unavailable.", cause);
    }

    ZLinkLocationStoreFailureException(String message, Throwable cause) {
        super(ZLinkFrameworkErrorKind.UNAVAILABLE, message, cause);
    }
}
