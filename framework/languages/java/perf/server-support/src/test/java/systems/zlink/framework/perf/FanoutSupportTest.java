package systems.zlink.framework.perf;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Collectors;
import java.util.stream.IntStream;

// §15.4 sequence originals: the bit set behind them, and the metric keys a PS role replaces.
class FanoutSupportTest {
    private static FanoutSupport.SequenceBitSet of(long... sequences) {
        FanoutSupport.SequenceBitSet set = new FanoutSupport.SequenceBitSet();
        for (long sequence : sequences) {
            set.trySet(sequence);
        }
        return set;
    }

    private static String show(List<FanoutSupport.SequenceRange> ranges) {
        return ranges.stream()
                .map(range -> range.first() + "-" + range.last())
                .collect(Collectors.joining(","));
    }

    @Test
    void rangesAreMaximalAcrossWordAndChunkBoundaries() {
        assertEquals(
                "5-7,9-9,63-65,262143-262144,1000000-1000000",
                show(of(5, 6, 7, 9, 63, 64, 65, 262143, 262144, 1_000_000).ranges()));
        FanoutSupport.SequenceBitSet full = new FanoutSupport.SequenceBitSet();
        for (long i = 0; i < 200; i++) {
            full.trySet(i);
        }
        assertEquals("0-199", show(full.ranges()));
        assertEquals("", show(new FanoutSupport.SequenceBitSet().ranges()));
        // A gap of one sequence must split the interval; a wrong boundary test would merge or shift
        // it.
        assertEquals("62-62,64-64", show(of(62, 64).ranges()));
        assertNotEquals("62-64", show(of(62, 64).ranges()));
    }

    @Test
    void trySetReportsDuplicatesAndCountsUniqueSequences() {
        FanoutSupport.SequenceBitSet set = new FanoutSupport.SequenceBitSet();
        assertTrue(set.trySet(10));
        assertFalse(set.trySet(10));
        assertTrue(set.contains(10));
        assertFalse(set.contains(11));
        assertEquals(1, set.count());
    }

    @Test
    void concurrentWritersLoseNoSequenceAndCountEachDuplicateOnce() {
        FanoutSupport.SequenceBitSet set = new FanoutSupport.SequenceBitSet();
        AtomicLong fresh = new AtomicLong();
        IntStream.range(0, 8)
                .parallel()
                .forEach(
                        worker -> {
                            for (long i = 0; i < 300_000; i++) {
                                if (set.trySet(i)) {
                                    fresh.incrementAndGet();
                                }
                            }
                        });
        assertEquals(300_000L, fresh.get());
        assertEquals(300_000L, set.count());
        assertEquals("0-299999", show(set.ranges()));
    }

    @Test
    void anOriginalCollisionFailsAndNeverReplacesTheFirstFile(@TempDir Path directory)
            throws Exception {
        Path path = directory.resolve("fanout.json");
        FanoutSupport.writeOnce(path, Map.of("value", 1));
        assertThrows(
                IllegalStateException.class,
                () -> FanoutSupport.writeOnce(path, Map.of("value", 2)));
        String text = Files.readString(path);
        assertTrue(text.contains("1"));
        assertFalse(text.contains("2"));
    }

    @Test
    void fanoutRolesReplaceEchoMetricsWithReasonedNullsAndTheEventRow() {
        RoleConfig config =
                new RoleConfig(
                        "test",
                        "pubsub-fanout-echo/1024/x",
                        "a".repeat(64),
                        "java",
                        "publisher",
                        0,
                        "pubsub-fanout-echo",
                        "publish",
                        "ordinary",
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        "",
                        "",
                        true,
                        "None",
                        true,
                        null,
                        List.of(),
                        List.of(),
                        null,
                        null,
                        null,
                        "Framework default",
                        new RoleConfig.Workload(
                                1024, .05, .05, null, 1, 1, null, 30000, 30000, 5000, 1000),
                        null,
                        Map.of());
        Measurement measurement = new Measurement(config, true);
        measurement.messageTypes(List.<String[]>of(new String[] {"event", "PerfPublishEvent"}));
        PerfSnapshot snapshot = measurement.snapshot(null);
        for (boolean owner : new boolean[] {true, false}) {
            FanoutSupport.applyCommon(snapshot, owner);
            for (String key :
                    List.of(
                            "messages.completed",
                            "throughput.kops",
                            "latency.p99Ms",
                            "fanout.deliveryRatio",
                            "fanout.deliveryLatency.p50Ms")) {
                assertNull(snapshot.metrics.get(key));
                assertFalse(snapshot.nullReasons.get("/metrics/" + key).reason().isEmpty());
            }
            assertEquals(
                    owner ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE",
                    snapshot.nullReasons.get("/metrics/fanout.deliveryLatency.p50Ms").code());
            snapshot.serializedMessageBytes.forEach(
                    row -> {
                        assertEquals("event", row.get("direction"));
                        assertEquals("PerfPublishEvent", row.get("packetName"));
                    });
            assertFalse(snapshot.nullReasons.containsKey("/histograms/latencyMs/maxNs"));
        }
        FanoutSupport.value(snapshot, "messages.publishedInWindow", "7");
        assertEquals("7", snapshot.metrics.get("messages.publishedInWindow"));
        assertFalse(snapshot.nullReasons.containsKey("/metrics/messages.publishedInWindow"));
    }
}
