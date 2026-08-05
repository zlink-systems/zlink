package systems.zlink.samples.bingo.server.api;

import java.util.concurrent.ConcurrentHashMap;
import org.springframework.stereotype.Component;

@Component
public final class BingoPlayerRecordStore {
    private final ConcurrentHashMap<String, PlayerRecord> records = new ConcurrentHashMap<>();

    public PlayerRecord get(String actorId) {
        return records.getOrDefault(actorId, new PlayerRecord(actorId, 0, 0));
    }

    public PlayerRecord report(String actorId, boolean won) {
        return records.compute(actorId, (ignored, current) -> {
            PlayerRecord record = current == null ? new PlayerRecord(actorId, 0, 0) : current;
            return won
                ? new PlayerRecord(actorId, record.wins() + 1, record.losses())
                : new PlayerRecord(actorId, record.wins(), record.losses() + 1);
        });
    }

    public record PlayerRecord(String actorId, int wins, int losses) {
    }
}
