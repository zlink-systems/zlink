package systems.zlink.framework.channels;

public interface ZLinkChannelRuntimeOptions {
    ZLinkClientServerChannelRuntimeOptions clientServerChannel(String channelName);

    ZLinkRouteMeshChannelRuntimeOptions routeMeshChannel(String channelName);
}
