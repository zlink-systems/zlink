package systems.zlink.samples.supportchat.server.support.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.support.application.ConversationAllocator;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.SupportChannel)
public final class AllocateConversationHandler
    implements ZLinkRequestHandler<Messages.AllocateConversationReq, Messages.AllocateConversationRes> {
    private final ConversationAllocator allocator;

    public AllocateConversationHandler(ConversationAllocator allocator) {
        this.allocator = allocator;
    }

    @Override
    public java.util.concurrent.CompletionStage<Messages.AllocateConversationRes> handle(
        Messages.AllocateConversationReq request,
        ZLinkMessageContext context) {
        return allocator.allocate(
                request.customerActorId(), request.customerDisplayName(), request.subject())
            .thenApply(conversationId -> new Messages.AllocateConversationRes(
                conversationId, SampleNames.Statuses.WaitingForAgent));
    }
}
