package systems.zlink.samples.supportchat.server.support.handlers;

import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.SupportChannel)
public final class EnsureSupportUserActorHandler
    implements ZLinkRequestHandler<Messages.EnsureSupportUserActorReq, Messages.EnsureSupportUserActorRes> {
    private final ZLinkActorManager actors;

    public EnsureSupportUserActorHandler(ZLinkActorManager actors) {
        this.actors = actors;
    }

    @Override
    public java.util.concurrent.CompletionStage<Messages.EnsureSupportUserActorRes> handle(
        Messages.EnsureSupportUserActorReq request,
        ZLinkMessageContext context) {
        return actors.getOrCreate(request.actorId(), SampleNames.SupportActorType)
            .request(request)
            .submit()
            .thenApply(result -> new Messages.EnsureSupportUserActorRes(
                ActorRefSnapshot.from(actorRef(result))));
    }

    private static systems.zlink.framework.actors.ActorRef actorRef(
        systems.zlink.framework.actors.ZLinkActorCreateResult result) {
        if (result instanceof systems.zlink.framework.actors.ZLinkActorCreateResult.Created created) {
            return created.actor();
        }
        if (result instanceof systems.zlink.framework.actors.ZLinkActorCreateResult.Existing existing) {
            return existing.actor();
        }
        throw new IllegalStateException("Support actor creation was rejected");
    }
}
