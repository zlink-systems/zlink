package systems.zlink.samples.supportchat.server.support.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(SampleNames.SupportChannel)
public final class EnsureAgentConversationHandler
        implements ZLinkRequestHandler<
                Messages.EnsureAgentConversationReq, Messages.EnsureAgentConversationRes> {
    private final ZLinkActorManager actors;

    public EnsureAgentConversationHandler(ZLinkActorManager actors) {
        this.actors = actors;
    }

    @Override
    public CompletionStage<Messages.EnsureAgentConversationRes> handle(
            Messages.EnsureAgentConversationReq request, ZLinkMessageContext context) {
        String conversationActorId = request.rosterActorId() + "@" + request.conversationId();
        return actors.find(conversationActorId)
                .thenCompose(
                        existing -> {
                            if (existing.isPresent()) {
                                ActorRef actorRef = existing.orElseThrow();
                                return java.util.concurrent.CompletableFuture.completedFuture(
                                        response(actorRef));
                            }
                            Messages.EnsureSupportUserActorReq create =
                                    new Messages.EnsureSupportUserActorReq(
                                            conversationActorId,
                                            request.displayName(),
                                            SampleNames.Roles.Agent,
                                            request.rosterActorId());
                            return actors.getOrCreate(
                                            conversationActorId, SampleNames.SupportActorType)
                                    .request(create)
                                    .submit()
                                    .thenApply(result -> response(actorRef(result)));
                        });
    }

    private static ActorRef actorRef(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Created created) {
            return created.actor();
        }
        if (result instanceof ZLinkActorCreateResult.Existing existing) {
            return existing.actor();
        }
        throw new IllegalStateException("Agent conversation actor creation was rejected");
    }

    private static Messages.EnsureAgentConversationRes response(ActorRef actorRef) {
        return new Messages.EnsureAgentConversationRes(ActorRefSnapshot.from(actorRef));
    }
}
