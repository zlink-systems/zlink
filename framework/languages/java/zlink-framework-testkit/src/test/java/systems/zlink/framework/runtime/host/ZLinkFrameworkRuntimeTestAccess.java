package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

/** Test-only access from the runtime implementation package. */
final class ZLinkFrameworkRuntimeTestAccess {
    private ZLinkFrameworkRuntimeTestAccess() {
    }

    static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendProvider) {
        return ZLinkFrameworkRuntime.start(options, backendProvider);
    }
}
