package systems.zlink.framework.configuration;

public interface ZLinkMeshChannelBuilder {
    ZLinkMeshChannelClientBuilder client();

    ZLinkMeshChannelServerBuilder server();
}
