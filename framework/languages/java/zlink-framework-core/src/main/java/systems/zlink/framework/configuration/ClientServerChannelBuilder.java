package systems.zlink.framework.configuration;

public interface ClientServerChannelBuilder {
    ZLinkClientServerChannelClientBuilder client();

    ZLinkClientServerChannelServerBuilder server();
}
