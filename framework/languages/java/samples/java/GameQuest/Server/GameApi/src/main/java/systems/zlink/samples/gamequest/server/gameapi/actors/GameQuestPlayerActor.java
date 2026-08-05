package systems.zlink.samples.gamequest.server.gameapi.actors;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class GameQuestPlayerActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;

    public GameQuestPlayerActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public CompletionStage<Void> push(Messages.QuestProcessingMsg message) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (Messages.QuestProgressNotify notification : message.progressNotifications()) {
            sends = sends.thenCompose(ignored ->
                context.boundSession().send(notification).submit());
        }
        for (Messages.QuestCompletedNotify notification : message.completedNotifications()) {
            sends = sends.thenCompose(ignored ->
                context.boundSession().send(notification).submit());
        }
        return sends;
    }
}
