package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

/** Test-only access to the host package's backend-injected bootstrap. */
public final class ZLinkFrameworkTestkitRuntimeAccess {
    private ZLinkFrameworkTestkitRuntimeAccess() {}

    public static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendFactory) {
        return ZLinkFrameworkRuntime.start(options, backendFactory);
    }
}
