package systems.zlink.samples.gamequest.server.questmission.domain;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class QuestDomain {
    public QuestDecision apply(
        Messages.GameplayMsg event,
        List<Messages.QuestProgress> current,
        long nextVersion) {
        long now = Instant.now().toEpochMilli();
        List<Messages.StoredQuestEvent> stored = new ArrayList<>();
        List<Messages.QuestProgressNotify> progress = new ArrayList<>();
        List<Messages.QuestCompletedNotify> completed = new ArrayList<>();
        List<Messages.QuestProgress> updated = new ArrayList<>(current);

        if ("kill".equals(event.eventType()) && "wolf".equals(event.value())) {
            applyCounter(event, updated, stored, progress, completed, Messages.QuestIds.FirstHunt, 3, event.count(), now, nextVersion);
        } else if ("collect".equals(event.eventType()) && "healing-herb".equals(event.value())) {
            applyCounter(event, updated, stored, progress, completed, Messages.QuestIds.HerbGathering, 5, event.count(), now, nextVersion);
        } else if ("feature".equals(event.eventType()) && "auction".equals(event.value())) {
            completeOnce(event, updated, stored, completed, Messages.QuestIds.OpenAuction, 1, now, nextVersion);
        }

        return new QuestDecision(updated, stored, progress, completed);
    }

    public Messages.QuestProgress fold(
        String playerId,
        String questId,
        List<Messages.StoredQuestEvent> events) {
        int current = 0;
        int required = 1;
        String status = Messages.QuestStatuses.InProgress;
        String lastEventId = null;
        long updatedAt = 0;
        List<Messages.StoredQuestEvent> ordered = events.stream()
            .sorted(Comparator.comparingLong(Messages.StoredQuestEvent::version))
            .toList();
        for (Messages.StoredQuestEvent event : ordered) {
            required = event.requiredCount();
            if (event.eventType().equals(Messages.QuestProgressedEvent.class.getSimpleName())) {
                current = Math.min(required, current + event.delta());
            } else if (event.eventType().equals(Messages.QuestProgressReconciledEvent.class.getSimpleName())) {
                current = event.currentCount();
                status = event.status();
            } else if (event.eventType().equals(Messages.QuestCompletedEvent.class.getSimpleName())) {
                current = Math.max(current, required);
            } else if (event.eventType().equals(Messages.QuestRewardGrantedEvent.class.getSimpleName())) {
                status = Messages.QuestStatuses.RewardGranted;
            }
            lastEventId = event.sourceEventId();
            updatedAt = Math.max(updatedAt, event.createdAtUnixMs());
        }
        return new Messages.QuestProgress(
            playerId,
            questId,
            status,
            current,
            required,
            lastEventId,
            updatedAt);
    }

    private static void applyCounter(
        Messages.GameplayMsg event,
        List<Messages.QuestProgress> projection,
        List<Messages.StoredQuestEvent> stored,
        List<Messages.QuestProgressNotify> progressNotifications,
        List<Messages.QuestCompletedNotify> completedNotifications,
        String questId,
        int required,
        int delta,
        long now,
        long nextVersion) {
        Messages.QuestProgress previous = find(projection, questId);
        int current = Math.min(required, (previous == null ? 0 : previous.currentCount()) + delta);
        String status = current >= required
            ? Messages.QuestStatuses.RewardGranted
            : Messages.QuestStatuses.InProgress;
        Messages.QuestProgress next = new Messages.QuestProgress(
            event.playerId(),
            questId,
            status,
            current,
            required,
            event.eventId(),
            now);
        replace(projection, next);
        if (previous == null || previous.currentCount() != current) {
            stored.add(new Messages.StoredQuestEvent(
                event.eventId() + ":progress",
                event.eventId(),
                event.playerId(),
                questId,
                Messages.QuestProgressedEvent.class.getSimpleName(),
                delta,
                next.currentCount(),
                next.requiredCount(),
                next.status(),
                nextVersion,
                now));
            if (event.publish()) {
                progressNotifications.add(new Messages.QuestProgressNotify(event.playerId(), null, next));
            }
        }
        if ((previous == null || !Messages.QuestStatuses.RewardGranted.equals(previous.status()))
            && Messages.QuestStatuses.RewardGranted.equals(status)) {
            stored.add(new Messages.StoredQuestEvent(
                event.eventId() + ":completed",
                event.eventId(),
                event.playerId(),
                questId,
                Messages.QuestCompletedEvent.class.getSimpleName(),
                0,
                next.currentCount(),
                next.requiredCount(),
                next.status(),
                nextVersion + 1,
                now));
            stored.add(new Messages.StoredQuestEvent(
                event.eventId() + ":reward",
                event.eventId(),
                event.playerId(),
                questId,
                Messages.QuestRewardGrantedEvent.class.getSimpleName(),
                0,
                next.currentCount(),
                next.requiredCount(),
                next.status(),
                nextVersion + 2,
                now));
            if (event.publish()) {
                completedNotifications.add(new Messages.QuestCompletedNotify(event.playerId(), null, next, true));
            }
        }
    }

    private static void completeOnce(
        Messages.GameplayMsg event,
        List<Messages.QuestProgress> projection,
        List<Messages.StoredQuestEvent> stored,
        List<Messages.QuestCompletedNotify> completedNotifications,
        String questId,
        int required,
        long now,
        long nextVersion) {
        Messages.QuestProgress previous = find(projection, questId);
        if (previous != null && Messages.QuestStatuses.RewardGranted.equals(previous.status())) {
            return;
        }
        Messages.QuestProgress next = new Messages.QuestProgress(
            event.playerId(),
            questId,
            Messages.QuestStatuses.RewardGranted,
            required,
            required,
            event.eventId(),
            now);
        replace(projection, next);
        stored.add(new Messages.StoredQuestEvent(
            event.eventId() + ":completed",
            event.eventId(),
            event.playerId(),
            questId,
            Messages.QuestCompletedEvent.class.getSimpleName(),
            0,
            next.currentCount(),
            next.requiredCount(),
            next.status(),
            nextVersion,
            now));
        stored.add(new Messages.StoredQuestEvent(
            event.eventId() + ":reward",
            event.eventId(),
            event.playerId(),
            questId,
            Messages.QuestRewardGrantedEvent.class.getSimpleName(),
            0,
            next.currentCount(),
            next.requiredCount(),
            next.status(),
            nextVersion + 1,
            now));
        if (event.publish()) {
            completedNotifications.add(new Messages.QuestCompletedNotify(event.playerId(), null, next, true));
        }
    }

    private static Messages.QuestProgress find(List<Messages.QuestProgress> projection, String questId) {
        return projection.stream()
            .filter(progress -> progress.questId().equals(questId))
            .findFirst()
            .orElse(null);
    }

    private static void replace(List<Messages.QuestProgress> projection, Messages.QuestProgress next) {
        projection.removeIf(progress -> progress.questId().equals(next.questId()));
        projection.add(next);
    }

    public record QuestDecision(
        List<Messages.QuestProgress> projection,
        List<Messages.StoredQuestEvent> storedEvents,
        List<Messages.QuestProgressNotify> progressNotifications,
        List<Messages.QuestCompletedNotify> completedNotifications) {
    }
}
