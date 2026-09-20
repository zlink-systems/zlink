package systems.zlink.tutorial.server.spots;

import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;

import java.util.ArrayList;
import java.util.List;

// --8<-- [start:instance-spot-class]
// Unlike a room, a match queue is never created explicitly. The first message
// addressed to a queue id brings it into being and is then handled by it.
// Players do not join it as members; it only processes requests, so there is
// no actor type to name and no create or join callback to write.
public final class MatchQueue implements ZLinkInstanceSpot {

    private final ZLinkInstanceSpotContext context;
    private final List<String> waiting = new ArrayList<>();

    public MatchQueue(ZLinkInstanceSpotContext context) {
        this.context = context;
        // Handlers are named here, the same way
        // the room names its own.
        context.handlers().addPacket(JoinMatchQueueHandler.class);
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    public int waiting() {
        return waiting.size();
    }

    public void enqueue(String playerId) {
        waiting.add(playerId);
    }
}
// --8<-- [end:instance-spot-class]
