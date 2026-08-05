package systems.zlink.samples.supportchat.server.support.spots.entryspot;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.actors.SupportActorDirectory;
import systems.zlink.samples.supportchat.server.support.application.AgentAssignmentService;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class SupportEntrySpot implements ZLinkEntrySpot<SupportUserActor> {
    private final ZLinkEntrySpotContext context;
    private final SupportActorDirectory directory;
    private final AgentAssignmentService assignment;

    public SupportEntrySpot(
        ZLinkEntrySpotContext context,
        SupportActorDirectory directory,
        AgentAssignmentService assignment) {
        this.context = context;
        this.directory = directory;
        this.assignment = assignment;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        SupportUserActor actor,
        ZLinkMessage createRequest) {
        Messages.EnsureSupportUserActorReq request =
            createRequest.decode(Messages.EnsureSupportUserActorReq.class);
        actor.setIdentity(request.displayName(), request.role(), request.participantId());
        directory.remember(actor);
        return java.util.concurrent.CompletableFuture.completedFuture(
            ZLinkActorCreateResponse.accept());
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinedActor(SupportUserActor actor) {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onLeaveActor(SupportUserActor actor) {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnectActor(SupportUserActor actor) {
        if (SampleNames.Roles.Agent.equals(actor.role())) {
            assignment.setAvailable(actor.participantId(), actor.displayName(), false);
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
