package systems.zlink.framework.configuration;

import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;

public interface ZLinkClientServerChannelServerBuilder {
    ZLinkClientServerChannelServerBuilder listen();

    ZLinkClientServerChannelServerBuilder listen(int port);

    ZLinkClientServerChannelServerBuilder setBindHost(String host);

    ZLinkClientServerChannelServerBuilder setAdvertiseHost(String host);

    ZLinkClientServerChannelServerBuilder setWeight(int weight);

    ZLinkClientServerChannelServerBuilder addHandlerGroup(String groupName);

    <THandler extends ZLinkSendHandler<TMessage>, TMessage>
    ZLinkClientServerChannelServerBuilder addSendHandler(
        Class<THandler> handlerType,
        Class<TMessage> messageType);

    <THandler extends ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply>
    ZLinkClientServerChannelServerBuilder addRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);
}
