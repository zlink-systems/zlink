/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.annotation.JsonPropertyOrder;

import java.util.ArrayList;
import java.util.List;

/** Mixed game-message body shared by the raw JSON and framework JSON bench rows. */
@JsonPropertyOrder({
    "displayName",
    "gold",
    "heading",
    "hp",
    "items",
    "level",
    "mp",
    "online",
    "phase",
    "playerId",
    "roomId",
    "run",
    "sentNs",
    "seq",
    "session",
    "x",
    "y",
    "z"
})
public record BenchGamePayload(
        String displayName,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long gold,
        int heading,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long hp,
        List<Item> items,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long level,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long mp,
        boolean online,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long phase,
        String playerId,
        String roomId,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long run,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long sentNs,
        @JsonFormat(shape = JsonFormat.Shape.STRING) long seq,
        String session,
        int x,
        int y,
        int z) {

    private static final long TIMESTAMP_OFFSET_NS = 1_000_000_000_000_000_000L;

    public static long timestampNow() {
        return TIMESTAMP_OFFSET_NS + BenchMetricHeader.nowNs();
    }

    public static BenchGamePayload create(
            int payloadSize, int runId, byte phase, long sequence, long sentNs) {
        int itemCount =
                switch (payloadSize) {
                    case 1024 -> 16;
                    case 4096 -> 88;
                    default ->
                            throw new IllegalArgumentException(
                                    "JSON bench implementation supports payload sizes 1024 and 4096: "
                                            + payloadSize);
                };
        List<Item> items = new ArrayList<>(itemCount);
        for (int index = 0; index < itemCount; index++) {
            int oneBased = index + 1;
            items.add(new Item(index % 9 + 1, "item-%08d".formatted(oneBased), index % 32));
        }
        return new BenchGamePayload(
                "Player_12345678",
                987654321L,
                271,
                287L,
                List.copyOf(items),
                42L,
                164L,
                true,
                phase,
                "player-0000000000000001",
                "room-000000000000000001",
                runId,
                sentNs,
                sequence,
                "session-0000000000000001",
                -123456,
                7890,
                654321);
    }

    public record Item(int count, String id, int slot) {}
}
