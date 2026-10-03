package systems.zlink.framework.perf;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;

// Perf spec §15.4: the sequence originals of the PS cells. The Publisher and Subscriber roles both use this file, so the
// original format has one definition.
public final class FanoutSupport {
    private FanoutSupport() {}

    public static final String TOPIC = "perf.echo";

    /** A §15.4 Range: both ends inclusive, canonical U64 text. */
    public record SequenceRange(String first, String last) {}

    public record PublisherSequences(String runId, String cellId, String resetSeq, String phase,
            List<SequenceRange> attemptedRanges, List<SequenceRange> windowSuccessRanges) {}

    public record SubscriberSequences(String runId, String cellId, String resetSeq, String phase, int subscriberId,
            List<SequenceRange> windowRanges, String duplicateEvents,
            Map<String, NullReason> nullReasons, List<Object> timingEvidence) {}

    /** A set of U64 sequences kept as a chunked bit set: one bit per sequence, safe for concurrent writers. */
    public static final class SequenceBitSet {
        private static final int CHUNK_BITS = 1 << 18;
        private static final int CHUNK_WORDS = CHUNK_BITS / 64;
        private final ConcurrentHashMap<Long, AtomicLongArray> chunks = new ConcurrentHashMap<>();
        private final AtomicLong count = new AtomicLong();

        public long count() {
            return count.get();
        }

        public long retainedBytes() {
            return (long) chunks.size() * CHUNK_WORDS * Long.BYTES;
        }

        /** False when the sequence was already in the set. */
        public boolean trySet(long sequence) {
            AtomicLongArray chunk = chunks.computeIfAbsent(sequence / CHUNK_BITS, key -> new AtomicLongArray(CHUNK_WORDS));
            int bit = (int) (sequence % CHUNK_BITS);
            long mask = 1L << (bit & 63);
            if ((chunk.getAndAccumulate(bit >> 6, mask, (a, b) -> a | b) & mask) != 0) {
                return false;
            }
            count.incrementAndGet();
            return true;
        }

        public boolean contains(long sequence) {
            AtomicLongArray chunk = chunks.get(sequence / CHUNK_BITS);
            if (chunk == null) {
                return false;
            }
            int bit = (int) (sequence % CHUNK_BITS);
            return (chunk.get(bit >> 6) & (1L << (bit & 63))) != 0;
        }

        /** Maximal contiguous intervals, ascending, both ends inclusive (§15.4). */
        public List<SequenceRange> ranges() {
            List<SequenceRange> ranges = new ArrayList<>();
            long open = -1;
            long next = 0;
            for (Map.Entry<Long, AtomicLongArray> entry : new TreeMap<>(chunks).entrySet()) {
                long origin = entry.getKey() * CHUNK_BITS;
                AtomicLongArray chunk = entry.getValue();
                for (int word = 0; word < CHUNK_WORDS; word++) {
                    long bits = chunk.get(word);
                    long position = origin + (long) word * 64;
                    while (bits != 0) {
                        int low = Long.numberOfTrailingZeros(bits);
                        long first = position + low;
                        if (open >= 0 && first == next) {
                            next++;
                        } else {
                            if (open >= 0) {
                                ranges.add(new SequenceRange(DecimalText.of(open), DecimalText.of(next - 1)));
                            }
                            open = first;
                            next = first + 1;
                        }
                        bits &= bits - 1;
                    }
                }
            }
            if (open >= 0) {
                ranges.add(new SequenceRange(DecimalText.of(open), DecimalText.of(next - 1)));
            }
            return ranges;
        }
    }

    private static final List<String> INTERSECTION_KEYS = List.of("fanout.subscriberCount",
            "fanout.deliveredInWindow", "fanout.outOfCohortEvents", "fanout.deliveryRatio",
            "fanout.deliveryOpsPerSec");
    private static final List<String> ECHO_KEYS = List.of("messages.completed", "throughput.kops");

    public static void value(PerfSnapshot snapshot, String key, Object value) {
        snapshot.metrics.put(key, value);
        snapshot.nullReasons.remove("/metrics/" + key);
    }

    public static void nullValue(PerfSnapshot snapshot, String key, String code, String reason) {
        MetricCatalog.nullValue(snapshot.metrics, snapshot.nullReasons, "metrics", key, code, reason);
    }

    /** Every PS role: the echo outcomes and echo latency do not apply (§10.11); delivery is intersected by the runner. */
    public static void applyCommon(PerfSnapshot snapshot, boolean hasDeliveryOwner) {
        for (String key : INTERSECTION_KEYS) {
            nullValue(snapshot, key, "NOT_APPLICABLE",
                    "Delivery counts come from the runner's intersection of the publisher and subscriber sequence originals (§15.4).");
        }
        for (String prefix : List.of("latency")) {
            for (String suffix : MetricCatalog.LATENCY_SUFFIXES) {
                nullValue(snapshot, prefix + "." + suffix, "NOT_APPLICABLE", "A fanout cell has no echo round trip (§10.11).");
            }
        }
        for (String key : List.of("latencyMs")) {
            MetricCatalog.nullValue(snapshot.histograms, snapshot.nullReasons, "histograms", key, "NOT_APPLICABLE",
                    "A fanout cell has no echo round trip (§10.11).");
            snapshot.nullReasons.remove("/histograms/" + key + "/maxNs");
        }
        for (String key : ECHO_KEYS) {
            nullValue(snapshot, key, "NOT_APPLICABLE", "A fanout cell records publish admission, not echo completion (§10.11).");
        }
        String latencyCode = hasDeliveryOwner ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE";
        for (String prefix : List.of("fanout.deliveryLatency")) {
            for (String suffix : MetricCatalog.LATENCY_SUFFIXES) {
                nullValue(snapshot, prefix + "." + suffix, latencyCode, hasDeliveryOwner
                        ? "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2)."
                        : "Delivery latency is observed by Subscriber processes.");
            }
        }
        for (String key : List.of("fanoutDeliveryLatencyMs")) {
            MetricCatalog.nullValue(snapshot.histograms, snapshot.nullReasons, "histograms", key, latencyCode, hasDeliveryOwner
                    ? "No verified shared clock domain between Publisher and Subscriber processes (§15.2)."
                    : "Delivery latency is observed by Subscriber processes.");
        }
    }

    /** The original of this cell is written once; an existing path is a collection collision. */
    public static void writeOnce(Path path, Object original) {
        try (OutputStream stream = Files.newOutputStream(path, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            stream.write((PerfJson.write(original) + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (FileAlreadyExistsException exists) {
            throw new IllegalStateException("Sequence original already exists: " + path, exists);
        } catch (IOException error) {
            throw new IllegalStateException("Cannot write " + path + ": " + error.getMessage(), error);
        }
    }
}
