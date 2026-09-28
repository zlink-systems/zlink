package systems.zlink.framework.runtime.host;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

final class ZLinkFrameworkChannelSubsystem {
    private final ZLinkChannelRuntime channels;

    private ZLinkFrameworkChannelSubsystem(ZLinkChannelRuntime channels) {
        this.channels = channels;
    }

    static ZLinkFrameworkChannelSubsystem create(
            DefaultZLinkFrameworkOptions options,
            ZLinkChannelBackendAdapter channelBackend,
            ZLinkBackendContext backendContext,
            ZLinkBackendAdapterProvider backendFactory,
            ZLinkBackendAdapterOptions adapterOptions,
            ZLinkMessageSerializer serializer,
            ZLinkHandlerActivator.MutableServices runtimeHandlers,
            ZLinkRuntimeEventDispatcher eventDispatcher) {
        backendContext.configureCoreHwm(options.registration().inboundDispatch());
        ZLinkChannelRuntime channels =
                new ZLinkChannelRuntime(
                        channelBackend,
                        backendContext,
                        backendFactory,
                        adapterOptions,
                        options.registration(),
                        serializer,
                        runtimeHandlers,
                        eventDispatcher);
        runtimeHandlers.add(ZLinkClient.class, channels);
        runtimeHandlers.add(ZLinkFanoutClient.class, channels);
        runtimeHandlers.add(ZLinkRouteClient.class, channels);
        return new ZLinkFrameworkChannelSubsystem(channels);
    }

    ZLinkChannelRuntime channels() {
        return channels;
    }
}
