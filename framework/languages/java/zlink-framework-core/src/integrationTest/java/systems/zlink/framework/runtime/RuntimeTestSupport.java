package systems.zlink.framework.runtime;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntimeTestAccess;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

final class RuntimeTestSupport {
    private RuntimeTestSupport() {}

    static ZLinkFrameworkRuntime startFramework(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendFactory) {
        return systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess.startHost(
                options, backendFactory);
    }

    /**
     * Reads the endpoint a legacy route channel ROUTER actually bound, straight from the router
     * that owns it. Legacy route channels are not public listeners, so listenerStatus does not
     * report them.
     */
    static String legacyRouteBoundEndpoint(ZLinkFrameworkRuntime runtime, String channelName) {
        return ZLinkChannelRuntimeTestAccess.requireRouteRouter(
                        systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess
                                .channels(runtime),
                        channelName)
                .lastEndpoint();
    }
}
