package systems.zlink.samples.gamequest.server.gameapi.store;

import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BooleanSupplier;
import systems.zlink.samples.gamequest.server.configuration.GameplayStateStore;
import systems.zlink.samples.gamequest.server.configuration.RedisSampleStore;
import systems.zlink.samples.gamequest.server.configuration.SampleTopology;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class GameQuestStore implements AutoCloseable {
    private final RedisSampleStore shared;
    private final GameplayStateStore gameplay;
    private final Map<String, List<Messages.QuestProgress>> projections = new HashMap<>();

    public GameQuestStore(SampleTopology topology) {
        shared = new RedisSampleStore(topology);
        gameplay = new GameplayStateStore(topology);
    }

    public synchronized void bind(String playerId, String apiName) {
        shared.bind(playerId, apiName);
    }

    public synchronized void unbind(String playerId) {
        shared.unbind(playerId);
    }

    public synchronized void recordGameplay(Messages.GameplayMsg event) {
        gameplay.record(event);
    }

    public synchronized void mergeProjection(String playerId, List<Messages.QuestProgress> projection) {
        projections.put(playerId, new ArrayList<>(projection));
        shared.writeProjection(playerId, projection);
    }

    public synchronized List<Messages.QuestProgress> projection(String playerId) {
        List<Messages.QuestProgress> sharedProjection = shared.readProjection(playerId);
        if (!sharedProjection.isEmpty()) {
            projections.put(playerId, new ArrayList<>(sharedProjection));
            return new ArrayList<>(sharedProjection);
        }
        return new ArrayList<>(projections.getOrDefault(playerId, List.of()));
    }

    public synchronized void deleteProjection(String playerId, String questId) {
        projections.computeIfAbsent(playerId, ignored -> new ArrayList<>())
            .removeIf(progress -> progress.questId().equals(questId));
        shared.writeProjection(playerId, projections.getOrDefault(playerId, List.of()));
    }

    public synchronized Messages.QuestProgress rebuildProjection(String playerId, String questId) {
        int current = gameplay.itemCount(playerId, "healing-herb");
        int required = Messages.QuestIds.HerbGathering.equals(questId) ? 5 : 3;
        Messages.QuestProgress rebuilt = new Messages.QuestProgress(
            playerId,
            questId,
            current >= required ? Messages.QuestStatuses.RewardGranted : Messages.QuestStatuses.InProgress,
            current,
            required,
            "rebuilt-" + Instant.now().toEpochMilli(),
            Instant.now().toEpochMilli());
        projections.computeIfAbsent(playerId, ignored -> new ArrayList<>()).removeIf(p -> p.questId().equals(questId));
        projections.get(playerId).add(rebuilt);
        shared.writeProjection(playerId, projections.get(playerId));
        return rebuilt;
    }

    public synchronized void addUnpublishedKill(String playerId) {
        gameplay.incrementKill(playerId, "wolf", 1);
    }

    public synchronized Messages.GetGameplaySnapshotRes snapshot(String playerId) {
        return gameplay.snapshot(playerId);
    }

    public synchronized Messages.GameQuestServerAssertRes assertState() {
        List<Messages.QuestProgress> alice = projection("player-alice");
        List<Messages.QuestProgress> bob = projection("player-bob");
        List<String> evidence = new ArrayList<>();
        alice.forEach(p -> evidence.add(p.playerId() + ":" + p.questId() + ":" + p.status() + ":"
            + p.currentCount() + "/" + p.requiredCount()));
        bob.forEach(p -> evidence.add(p.playerId() + ":" + p.questId() + ":" + p.status() + ":"
            + p.currentCount() + "/" + p.requiredCount()));
        List<String> bindingHistory = shared.bindingHistory();
        List<String> activeBindings = shared.activeBindings();
        List<Messages.StoredQuestEvent> events = shared.readQuestEvents();
        List<String> deduplicatedEvents = shared.deduplicatedEvents();
        bindingHistory.forEach(binding -> evidence.add("binding:" + binding));
        events.forEach(event -> evidence.add("event:" + event.playerId() + ":" + event.questId()
            + ":" + event.eventType() + ":v" + event.version() + ":source=" + event.sourceEventId()));
        deduplicatedEvents.forEach(eventId -> evidence.add("deduplicated:" + eventId));

        List<BooleanSupplier> conditions = List.of(
            check(evidence, "missing:player-alice:first-hunt:RewardGranted", () -> alice.stream().anyMatch(p ->
                p.questId().equals(Messages.QuestIds.FirstHunt)
                    && p.status().equals(Messages.QuestStatuses.RewardGranted))),
            check(evidence, "missing:player-alice:open-auction:RewardGranted", () -> alice.stream().anyMatch(p ->
                p.questId().equals(Messages.QuestIds.OpenAuction)
                    && p.status().equals(Messages.QuestStatuses.RewardGranted))),
            check(evidence, "missing:player-bob:herb-gathering:RewardGranted", () -> bob.stream().anyMatch(p ->
                p.questId().equals(Messages.QuestIds.HerbGathering)
                    && p.status().equals(Messages.QuestStatuses.RewardGranted))),
            check(evidence, "missing:binding:player-bob:api-b", () -> bindingHistory.contains("player-bob:api-b")),
            check(evidence, "missing:binding:player-alice:api-b", () -> bindingHistory.contains("player-alice:api-b")),
            check(evidence, "unexpected:active-binding:player-alice", () -> !activeBindings.contains("player-alice")),
            check(evidence, "missing:event:player-alice:first-hunt:QuestProgressedEvent:3",
                () -> count(events, "player-alice", Messages.QuestIds.FirstHunt,
                    Messages.QuestProgressedEvent.class.getSimpleName()) == 3),
            check(evidence, "invalid:player-alice:first-hunt:progress-delta-fold",
                () -> progressDelta(events, "player-alice", Messages.QuestIds.FirstHunt) == 3),
            check(evidence, "missing:event:player-alice:first-hunt:QuestCompletedEvent:1",
                () -> count(events, "player-alice", Messages.QuestIds.FirstHunt,
                    Messages.QuestCompletedEvent.class.getSimpleName()) == 1),
            check(evidence, "missing:event:player-alice:first-hunt:QuestRewardGrantedEvent:1",
                () -> count(events, "player-alice", Messages.QuestIds.FirstHunt,
                    Messages.QuestRewardGrantedEvent.class.getSimpleName()) == 1),
            check(evidence, "missing:deduplicated:player-alice-kill-3",
                () -> deduplicatedEvents.contains("player-alice-kill-3")),
            check(evidence, "missing:event:player-alice:first-hunt:QuestProgressReconciledEvent:1",
                () -> count(events, "player-alice", Messages.QuestIds.FirstHunt,
                    Messages.QuestProgressReconciledEvent.class.getSimpleName()) == 1),
            check(evidence, "missing:event:player-alice:open-auction:QuestCompletedEvent:1",
                () -> count(events, "player-alice", Messages.QuestIds.OpenAuction,
                    Messages.QuestCompletedEvent.class.getSimpleName()) == 1),
            check(evidence, "missing:event:player-alice:open-auction:QuestRewardGrantedEvent:1",
                () -> count(events, "player-alice", Messages.QuestIds.OpenAuction,
                    Messages.QuestRewardGrantedEvent.class.getSimpleName()) == 1),
            check(evidence, "missing:event:player-bob:herb-gathering:QuestCompletedEvent:1",
                () -> count(events, "player-bob", Messages.QuestIds.HerbGathering,
                    Messages.QuestCompletedEvent.class.getSimpleName()) == 1),
            check(evidence, "missing:event:player-bob:herb-gathering:QuestRewardGrantedEvent:1",
                () -> count(events, "player-bob", Messages.QuestIds.HerbGathering,
                    Messages.QuestRewardGrantedEvent.class.getSimpleName()) == 1),
            check(evidence, "duplicate:event-version", () -> uniqueEventVersions(events)));
        boolean passed = true;
        for (BooleanSupplier condition : conditions) {
            passed &= condition.getAsBoolean();
        }
        return new Messages.GameQuestServerAssertRes(passed, evidence.stream().sorted().toList());
    }

    @Override
    public void close() {
        gameplay.close();
        shared.close();
    }

    private static BooleanSupplier check(List<String> evidence, String failure, BooleanSupplier condition) {
        return () -> {
            boolean passed = condition.getAsBoolean();
            if (!passed) {
                evidence.add("failure:" + failure);
            }
            return passed;
        };
    }

    private static long count(
        List<Messages.StoredQuestEvent> events,
        String playerId,
        String questId,
        String eventType) {
        return events.stream()
            .filter(event -> event.playerId().equals(playerId)
                && event.questId().equals(questId)
                && event.eventType().equals(eventType))
            .count();
    }

    private static boolean uniqueEventVersions(List<Messages.StoredQuestEvent> events) {
        return events.stream()
            .map(event -> event.playerId() + ":" + event.questId() + ":" + event.version())
            .distinct()
            .count() == events.size();
    }

    private static int progressDelta(
        List<Messages.StoredQuestEvent> events,
        String playerId,
        String questId) {
        return events.stream()
            .filter(event -> event.playerId().equals(playerId)
                && event.questId().equals(questId)
                && event.eventType().equals(Messages.QuestProgressedEvent.class.getSimpleName()))
            .mapToInt(Messages.StoredQuestEvent::delta)
            .sum();
    }
}
