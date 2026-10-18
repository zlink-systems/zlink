package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * A Location Store/provider or conditional-write failure. These boundaries preserve the Store
 * origin for host relocation spec 30 §13 {@code StoreUnavailable}.
 */
public final class ZLinkLocationStoreFailureException extends ZLinkFrameworkException
        implements ZLinkLocationStoreFailure {
    ZLinkLocationStoreFailureException(Throwable cause) {
        this(ZLinkFrameworkErrorKind.UNAVAILABLE, "Location Store provider is unavailable.", cause);
    }

    ZLinkLocationStoreFailureException(
            ZLinkFrameworkErrorKind kind, String message, Throwable cause) {
        super(kind, message, cause);
    }
}
