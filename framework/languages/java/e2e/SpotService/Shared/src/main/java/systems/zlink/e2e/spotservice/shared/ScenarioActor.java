package systems.zlink.e2e.spotservice.shared;

import java.util.List;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

public final class ScenarioActor implements ZLinkActor {
    private final ZLinkActorContext context;
    private Contracts.ActorProfile profile = new Contracts.ActorProfile("", 0, List.of());
    private int sequence;

    public ScenarioActor(
        ZLinkActorContext context) {
        this.context = context;
    }

    public String actorId() {
        return context.actorId();
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public int nextSequence() {
        sequence++;
        return sequence;
    }

    public int currentSequence() {
        return sequence;
    }

    public void applyProfile(Contracts.ActorProfile next) {
        profile = next;
    }

    public Contracts.ActorProfile profile() {
        return profile;
    }
}
