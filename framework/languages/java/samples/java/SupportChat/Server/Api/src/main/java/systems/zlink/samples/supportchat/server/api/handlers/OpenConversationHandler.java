package systems.zlink.samples.supportchat.server.api.handlers;

import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTimings;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.ApiChannel)
public final class OpenConversationHandler
    implements ZLinkRequestHandler<Messages.OpenConversationApiReq, Messages.OpenConversationApiRes> {
    private final ZLinkClient channels;

    public OpenConversationHandler(ZLinkClient channels) {
        this.channels = channels;
    }

    @Override
    public java.util.concurrent.CompletionStage<Messages.OpenConversationApiRes> handle(
        Messages.OpenConversationApiReq request,
        ZLinkMessageContext context) {
        return channels.requestToChannel(
                SampleNames.SupportChannel,
                new Messages.AllocateConversationReq(
                    request.customerActorId(),
                    request.customerDisplayName(),
                    request.subject()))
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.AllocateConversationRes.class)
            .thenApply(allocated -> new Messages.OpenConversationApiRes(
                allocated.conversationId(), allocated.status()));
    }
}
