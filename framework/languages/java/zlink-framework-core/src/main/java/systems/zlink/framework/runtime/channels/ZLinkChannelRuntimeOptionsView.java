package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;

final class DefaultClientServerChannelRuntimeOptions
    implements ZLinkClientServerChannelRuntimeOptions {
    private final ZLinkChannelRuntime host;
    private final String channelName;

    DefaultClientServerChannelRuntimeOptions(ZLinkChannelRuntime host, String channelName) {
        this.host = host;
        this.channelName = channelName;
    }

    @Override
    public ZLinkSocketRuntimeOptions configureServerSocket() {
        return new DefaultChannelSocketRuntimeOptions(host, channelName);
    }
}

final class DefaultRouteMeshChannelRuntimeOptions
    implements ZLinkRouteMeshChannelRuntimeOptions {
    private final ZLinkChannelRuntime host;
    private final String channelName;

    DefaultRouteMeshChannelRuntimeOptions(ZLinkChannelRuntime host, String channelName) {
        this.host = host;
        this.channelName = channelName;
    }

    @Override
    public ZLinkSocketRuntimeOptions configureServerSocket() {
        return new DefaultChannelSocketRuntimeOptions(host, channelName);
    }
}

final class DefaultChannelSocketRuntimeOptions implements ZLinkSocketRuntimeOptions {
    private final ZLinkChannelRuntime host;
    private final String channelName;

    DefaultChannelSocketRuntimeOptions(ZLinkChannelRuntime host, String channelName) {
        this.host = host;
        this.channelName = channelName;
    }

    @Override
    public long maxMessageSize() {
        return host.serverSocket(channelName).maxMessageSize();
    }

    @Override
    public void maxMessageSize(long value) {
        if (value < 0) {
            throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                "MaxMessageSize must be zero or a positive byte count.");
        }
        host.serverSocket(channelName).setMaxMessageSize(value);
    }

    @Override
    public int weight() {
        return host.serverSocket(channelName).peerWeight();
    }

    @Override
    public void weight(int value) {
        ZLinkChannelRuntime.validatePeerWeight(value);
        host.serverSocket(channelName).setPeerWeight(value);
    }
}
