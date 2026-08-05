package systems.zlink.samples.gamequest.server.questmission.store;

import java.time.Instant;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.samples.gamequest.server.configuration.GameplayStateStore;
import systems.zlink.samples.gamequest.server.configuration.RedisSampleStore;
import systems.zlink.samples.gamequest.server.configuration.SampleTopology;
import systems.zlink.samples.gamequest.server.questmission.domain.QuestDomain;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class QuestStore implements AutoCloseable {
    private final QuestDomain domain = new QuestDomain();
    private final RedisSampleStore shared;
    private final GameplayStateStore gameplay;
    private final Map<String, PlayerState> states = new ConcurrentHashMap<>();

    public QuestStore(SampleTopology topology) {
        shared = new RedisSampleStore(topology);
        gameplay = new GameplayStateStore(topology);
    }

    public void activate(String playerId) {
        state(playerId);
    }

    public Messages.QuestProcessingMsg apply(Messages.GameplayMsg event) {
        PlayerState state = state(event.playerId());
        if (state.appliedEventIds.contains(event.eventId())) {
            shared.recordDeduplicatedEvent(event.eventId());
            return new Messages.QuestProcessingMsg(
                event.eventId(),
                event.playerId(),
                copyProjection(state),
                List.of(),
                List.of(),
                true);
        }
        List<Messages.QuestProgress> current = copyProjection(state);
        QuestDomain.QuestDecision decision = domain.apply(event, current, nextVersion(state));
        state.projection = new ArrayList<>(decision.projection());
        state.events.addAll(decision.storedEvents());
        shared.writeProjection(event.playerId(), decision.projection());
        shared.appendQuestEvents(decision.storedEvents());
        state.appliedEventIds.add(event.eventId());
        return new Messages.QuestProcessingMsg(
            event.eventId(),
            event.playerId(),
            copyProjection(state),
            decision.progressNotifications(),
            decision.completedNotifications(),
            false);
    }

    public Messages.SyncQuestProgressRes sync(String playerId) {
        PlayerState state = state(playerId);
        int firstHuntCount = gameplay.killCount(playerId, "wolf");
        List<Messages.QuestProgress> projection = copyProjection(state);
        Messages.QuestProgress firstHunt = projection.stream()
            .filter(progress -> progress.questId().equals(Messages.QuestIds.FirstHunt))
            .findFirst()
            .orElse(null);
        if (firstHunt == null || firstHunt.currentCount() < firstHuntCount) {
            long now = Instant.now().toEpochMilli();
            Messages.QuestProgress reconciled = new Messages.QuestProgress(
                playerId,
                Messages.QuestIds.FirstHunt,
                firstHuntCount >= 3 ? Messages.QuestStatuses.RewardGranted : Messages.QuestStatuses.InProgress,
                firstHuntCount,
                3,
                "sync-" + now,
                now);
            projection.removeIf(progress -> progress.questId().equals(Messages.QuestIds.FirstHunt));
            projection.add(reconciled);
            state.projection = projection;
            Messages.StoredQuestEvent reconciledEvent = new Messages.StoredQuestEvent(
                "sync-" + now,
                null,
                playerId,
                Messages.QuestIds.FirstHunt,
                Messages.QuestProgressReconciledEvent.class.getSimpleName(),
                0,
                reconciled.currentCount(),
                reconciled.requiredCount(),
                reconciled.status(),
                nextVersion(state),
                now);
            state.events.add(reconciledEvent);
            shared.writeProjection(playerId, projection);
            shared.appendQuestEvents(List.of(reconciledEvent));
        }
        return new Messages.SyncQuestProgressRes(copyProjection(state));
    }

    public Messages.DeleteQuestProjectionRes deleteProjection(String playerId, String questId) {
        PlayerState state = state(playerId);
        state.projection.removeIf(progress -> progress.questId().equals(questId));
        shared.writeProjection(playerId, state.projection);
        return new Messages.DeleteQuestProjectionRes(true);
    }

    public Messages.QuestProgress rebuildProjection(String playerId, String questId, int count) {
        PlayerState state = state(playerId);
        List<Messages.StoredQuestEvent> stream = state.events.stream()
            .filter(event -> event.questId().equals(questId))
            .toList();
        if (stream.isEmpty()) {
            throw new IllegalStateException("Quest stream was not found for " + playerId + "/" + questId);
        }

        Messages.QuestProgress rebuilt = domain.fold(playerId, questId, stream);
        state.projection.removeIf(progress -> progress.questId().equals(questId));
        state.projection.add(rebuilt);
        shared.writeProjection(playerId, state.projection);
        return rebuilt;
    }

    public List<Messages.QuestProgress> projection(String playerId) {
        return copyProjection(state(playerId));
    }

    public List<Messages.StoredQuestEvent> events() {
        // The endpoint is evidence for durable replay, so read the shared
        // event stream instead of exposing only owners activated in this JVM.
        return shared.readQuestEvents();
    }

    public Set<String> players() {
        return new HashSet<>(states.keySet());
    }

    @Override
    public void close() {
        gameplay.close();
        shared.close();
    }

    private static long nextVersion(PlayerState state) {
        return state.events.size() + 1L;
    }

    private static List<Messages.QuestProgress> copyProjection(PlayerState state) {
        return new ArrayList<>(state.projection);
    }

    private PlayerState state(String playerId) {
        return states.computeIfAbsent(playerId, this::restorePlayer);
    }

    private PlayerState restorePlayer(String playerId) {
        List<Messages.StoredQuestEvent> stored = shared.readQuestEvents().stream()
            .filter(event -> event.playerId().equals(playerId))
            .toList();
        PlayerState state = new PlayerState();
        state.events.addAll(stored);
        Set<String> questIds = new HashSet<>();
        for (Messages.StoredQuestEvent event : stored) {
            questIds.add(event.questId());
            if (event.sourceEventId() != null) {
                state.appliedEventIds.add(event.sourceEventId());
            }
        }
        for (String questId : questIds) {
            List<Messages.StoredQuestEvent> stream = stored.stream()
                .filter(event -> event.questId().equals(questId))
                .toList();
            state.projection.add(domain.fold(playerId, questId, stream));
        }
        return state;
    }

    private static final class PlayerState {
        private List<Messages.QuestProgress> projection = new ArrayList<>();
        private final Set<String> appliedEventIds = new HashSet<>();
        private final List<Messages.StoredQuestEvent> events = new ArrayList<>();
    }
}
