package systems.zlink.framework.configuration;

import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;

public interface ZLinkMeshChannelServerBuilder {
    ZLinkMeshChannelServerBuilder setWeight(int weight);

    ZLinkMeshChannelServerBuilder addHandlerGroup(String groupName);

    <THandler extends ZLinkSendHandler<TMessage>, TMessage>
    ZLinkMeshChannelServerBuilder addSendHandler(
        Class<THandler> handlerType,
        Class<TMessage> messageType);

    <THandler extends ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply>
    ZLinkMeshChannelServerBuilder addRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);
}
