package systems.zlink.framework.runtime.host;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

final class ZLinkFrameworkChannelSubsystem {
    private final ZLinkChannelRuntime channels;
    private final ZLinkBackendContext backendContext;

    private ZLinkFrameworkChannelSubsystem(
        ZLinkChannelRuntime channels,
        ZLinkBackendContext backendContext) {
        this.channels = channels;
        this.backendContext = backendContext;
    }

    static ZLinkFrameworkChannelSubsystem create(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator.MutableServices runtimeHandlers,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        ZLinkChannelBackendAdapter channelBackend = backendFactory.createChannelAdapter(adapterOptions);
        ZLinkBackendContext backendContext = channelBackend.createContext();
        ZLinkChannelRuntime channels = new ZLinkChannelRuntime(
            channelBackend,
            backendContext,
            backendFactory,
            adapterOptions,
            options.registration(),
            serializer,
            runtimeHandlers,
            eventDispatcher,
            ZLinkAdmissionRuntime.factory(backendFactory));
        runtimeHandlers.add(ZLinkClient.class, channels);
        runtimeHandlers.add(ZLinkFanoutClient.class, channels);
        runtimeHandlers.add(ZLinkRouteClient.class, channels);
        return new ZLinkFrameworkChannelSubsystem(channels, backendContext);
    }

    ZLinkChannelRuntime channels() {
        return channels;
    }

    ZLinkBackendContext backendContext() {
        return backendContext;
    }
}
