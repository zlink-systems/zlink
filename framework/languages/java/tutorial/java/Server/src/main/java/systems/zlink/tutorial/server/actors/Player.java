package systems.zlink.tutorial.server.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

// --8<-- [start:actor-class]
// A player is addressed by its own id and carries state that outlives any one
// connection. Like a room, its messages run one at a time.
public final class Player implements ZLinkActor {

    private final ZLinkActorContext context;

    private String nickname = "anonymous";

    public Player(ZLinkActorContext context) {
        this.context = context;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public String nickname() {
        return nickname;
    }

    public void rename(String value) {
        nickname = value;
    }
}
// --8<-- [end:actor-class]
