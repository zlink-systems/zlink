package systems.zlink.samples.supportchat.server.support.actors;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class SupportActorDirectory {
    private final Map<String, SupportUserActor> actors = new HashMap<>();
    private final Map<String, List<String>> conversations = new HashMap<>();

    public synchronized void remember(SupportUserActor actor) {
        actors.put(actor.actorId(), actor);
    }

    public synchronized SupportUserActor require(String actorId) {
        SupportUserActor actor = actors.get(actorId);
        if (actor == null) {
            throw new IllegalStateException("support actor not found: " + actorId);
        }
        return actor;
    }

    public synchronized void remove(String actorId) {
        actors.remove(actorId);
        conversations.values().forEach(actorIds -> actorIds.remove(actorId));
    }

    public synchronized void join(String conversationId, SupportUserActor actor) {
        remember(actor);
        conversations.computeIfAbsent(conversationId, ignored -> new ArrayList<>());
        List<String> actorIds = conversations.get(conversationId);
        if (!actorIds.contains(actor.actorId())) {
            actorIds.add(actor.actorId());
        }
    }

    public synchronized List<SupportUserActor> participants(String conversationId) {
        return conversations.getOrDefault(conversationId, List.of()).stream()
            .map(actors::get)
            .filter(actor -> actor != null)
            .toList();
    }
}
