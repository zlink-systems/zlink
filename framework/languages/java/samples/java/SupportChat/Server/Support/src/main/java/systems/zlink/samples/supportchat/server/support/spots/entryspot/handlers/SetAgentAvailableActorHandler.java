package systems.zlink.samples.supportchat.server.support.spots.entryspot.handlers;

import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.support.actors.SupportActorDirectory;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.application.AgentAssignmentService;
import systems.zlink.samples.supportchat.server.support.spots.entryspot.SupportEntrySpot;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class SetAgentAvailableActorHandler
    implements ZLinkEntrySpotActorRequestHandler<
        SupportEntrySpot,
        SupportUserActor,
        Messages.SetAgentAvailableReq,
        Messages.SetAgentAvailableRes> {
    private final AgentAssignmentService assignment;
    private final SupportActorDirectory directory;

    public SetAgentAvailableActorHandler(
        AgentAssignmentService assignment,
        SupportActorDirectory directory) {
        this.assignment = assignment;
        this.directory = directory;
    }

    @Override
    public java.util.concurrent.CompletionStage<Messages.SetAgentAvailableRes> handle(
        SupportEntrySpot spot,
        SupportUserActor actor,
        ZLinkMessageContext context,
        Messages.SetAgentAvailableReq request) {
        requireRole(actor, SampleNames.Roles.Agent);
        directory.remember(actor);
        assignment.setAvailable(
            actor.participantId(), actor.displayName(), request.isAvailable());
        return java.util.concurrent.CompletableFuture.completedFuture(
            new Messages.SetAgentAvailableRes(request.isAvailable()));
    }

    private static void requireRole(SupportUserActor actor, String expectedRole) {
        if (!expectedRole.equals(actor.role())) {
            throw new IllegalStateException("Expected role " + expectedRole + " but got " + actor.role());
        }
    }
}
