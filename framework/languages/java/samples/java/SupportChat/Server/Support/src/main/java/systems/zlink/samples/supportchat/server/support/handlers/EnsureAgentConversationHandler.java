package systems.zlink.samples.supportchat.server.support.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTimings;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.SupportChannel)
public final class EnsureAgentConversationHandler
    implements ZLinkRequestHandler<Messages.EnsureAgentConversationReq,
        Messages.EnsureAgentConversationRes> {
    private final ZLinkActorClient actorClient;
    private final ZLinkActorManager actors;

    public EnsureAgentConversationHandler(
        ZLinkActorClient actorClient,
        ZLinkActorManager actors) {
        this.actorClient = actorClient;
        this.actors = actors;
    }

    @Override
    public CompletionStage<Messages.EnsureAgentConversationRes> handle(
        Messages.EnsureAgentConversationReq request,
        ZLinkMessageContext context) {
        String conversationActorId = request.rosterActorId() + "@" + request.conversationId();
        return actors.find(conversationActorId).thenCompose(existing -> {
            if (existing.isPresent()) {
                ActorRef actorRef = existing.orElseThrow();
                return refresh(actorRef).thenApply(joined -> response(actorRef, joined));
            }
            Messages.EnsureSupportUserActorReq create = new Messages.EnsureSupportUserActorReq(
                conversationActorId,
                request.displayName(),
                SampleNames.Roles.Agent,
                request.rosterActorId());
            return actors.getOrCreate(conversationActorId, SampleNames.SupportActorType)
                .request(create)
                .submit()
                .thenCompose(result -> {
                    ActorRef actorRef = actorRef(result);
                    return actorClient.requestToActor(
                            actorRef.actorId(),
                            new Messages.JoinConversationReq(
                                request.rosterActorId(),
                                SampleNames.Roles.Agent,
                                request.displayName()))
                        .metadata(
                            SampleNames.ConversationIdMetadataKey,
                            request.conversationId())
                        .timeout(SampleTimings.RequestTimeout)
                        .submit(Messages.JoinConversationRes.class)
                        .thenApply(joined -> response(actorRef, joined));
                });
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

    private CompletionStage<Messages.JoinConversationRes> refresh(ActorRef actorRef) {
        return actorClient.requestToActor(actorRef.actorId(), new Messages.JoinConversationReq())
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.JoinConversationRes.class);
    }

    private static Messages.EnsureAgentConversationRes response(
        ActorRef actorRef,
        Messages.JoinConversationRes joined) {
        return new Messages.EnsureAgentConversationRes(
            ActorRefSnapshot.from(actorRef), joined.scheduled(), joined.state());
    }
}
