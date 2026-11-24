package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.lettuce.core.FlushMode;
import io.lettuce.core.RedisClient;
import io.lettuce.core.RedisCommandExecutionException;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreValueCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.io.IOException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.regex.Pattern;

class ZLinkRedisOpaqueLocationStoreTest {
    @Test
    void reusesCachedScriptsAndRecoversAfterScriptFlush() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        String prefix = "script-cache:" + UUID.randomUUID();
        var key = new ZLinkStoreKey("descriptor/cache");
        var write =
                new ZLinkStoreWriteRequest(
                        List.of(), List.of(new ZLinkStorePut(key, new byte[] {1}, null)));
        var keys = new ZLinkRedisLocationKeys(prefix);
        String sequenceKey = keys.opaqueSequenceKey();
        try (var client = RedisClient.create(endpoint);
                var connection = client.connect();
                var store =
                        new ZLinkRedisLocationStore(
                                new ZLinkRedisLocationOptions()
                                        .setConnectionString(endpoint)
                                        .setKeyPrefix(prefix))) {
            var commands = connection.sync();
            exerciseOpaqueScripts(store, key, write);
            String warmed = commands.info("commandstats");

            for (int repeat = 0; repeat < 3; repeat++) {
                exerciseOpaqueScripts(store, key, write);
            }
            String repeated = commands.info("commandstats");
            assertScriptCalls("warm", warmed, repeated, 0, 9);

            long sequence = Long.parseLong(commands.get(sequenceKey));
            commands.scriptFlush(FlushMode.SYNC);
            exerciseOpaqueScripts(store, key, write);
            String recovered = commands.info("commandstats");
            assertScriptCalls("after SCRIPT FLUSH", repeated, recovered, 3, 3);
            long recoveredSequence = Long.parseLong(commands.get(sequenceKey));
            assertEquals(sequence + 1, recoveredSequence);
            System.out.printf(
                    "Write sequence after SCRIPT FLUSH: %d -> %d%n", sequence, recoveredSequence);

            exerciseOpaqueScripts(store, key, write);
            String rewarmed = commands.info("commandstats");
            assertScriptCalls("rewarmed", recovered, rewarmed, 0, 3);

            commands.set(keys.opaqueRecordKey(key.value()), "invalid record type");
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () -> store.read(key, () -> false).toCompletableFuture().get());
            assertInstanceOf(RedisCommandExecutionException.class, failure.getCause());
            assertTrue(failure.getCause().getMessage().startsWith("WRONGTYPE"));
            String failed = commands.info("commandstats");
            assertScriptCalls("WRONGTYPE", rewarmed, failed, 0, 1);
        }
    }

    private static void assertScriptCalls(
            String phase, String before, String after, long expectedEval, long expectedEvalSha) {
        long eval = scriptCalls(after, "eval") - scriptCalls(before, "eval");
        long evalSha = scriptCalls(after, "evalsha") - scriptCalls(before, "evalsha");
        System.out.printf("Redis commandstats %s: EVAL %+d, EVALSHA %+d%n", phase, eval, evalSha);
        assertEquals(expectedEval, eval, phase + " EVAL");
        assertEquals(expectedEvalSha, evalSha, phase + " EVALSHA");
    }

    private static void exerciseOpaqueScripts(
            ZLinkRedisLocationStore store, ZLinkStoreKey key, ZLinkStoreWriteRequest write)
            throws Exception {
        assertInstanceOf(
                ZLinkStoreWriteApplied.class,
                store.write(write, () -> false).toCompletableFuture().get());
        assertInstanceOf(
                ZLinkStoreReadFound.class,
                store.read(key, () -> false).toCompletableFuture().get());
        assertInstanceOf(
                ZLinkStoreScanPageResult.class,
                store.scan(new ZLinkStoreScanRequest("descriptor/", null, 1), () -> false)
                        .toCompletableFuture()
                        .get());
    }

    private static long scriptCalls(String commandStats, String command) {
        var matcher =
                Pattern.compile("(?m)^cmdstat_" + command + ":calls=(\\d+),").matcher(commandStats);
        return matcher.find() ? Long.parseLong(matcher.group(1)) : 0;
    }

    @Test
    void completedSnapshotAllowsImmediateHistoryReclamation() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("history-scan:" + UUID.randomUUID()))) {
            var firstKey = new ZLinkStoreKey("hot/a");
            var hotKey = new ZLinkStoreKey("hot/b");
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(firstKey, new byte[] {1}, null),
                                            new ZLinkStorePut(hotKey, new byte[] {2}, null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest("hot/", null, 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            var writes = new ArrayList<CompletableFuture<?>>();
            for (int index = 0; index < 127; index++) {
                writes.add(
                        store.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                hotKey, new byte[] {3}, null))),
                                        () -> false)
                                .toCompletableFuture());
            }
            CompletableFuture.allOf(writes.toArray(CompletableFuture[]::new)).get();
            var overflow =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    store.write(
                                                    new ZLinkStoreWriteRequest(
                                                            List.of(),
                                                            List.of(
                                                                    new ZLinkStorePut(
                                                                            hotKey,
                                                                            new byte[] {4},
                                                                            null))),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get());
            assertInstanceOf(IOException.class, overflow.getCause());
            var last =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "hot/", first.nextCursor(), 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertArrayEquals(new byte[] {2}, last.items().getFirst().value().bytes());
            assertNull(last.nextCursor());
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            hotKey, new byte[] {4}, null))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
        }
    }

    @Test
    void prefixRangeIncludesExactAndUtf8KeysAcrossPages() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("utf8-scan:" + UUID.randomUUID()))) {
            var exact = new ZLinkStoreKey("é");
            var suffix = new ZLinkStoreKey("é/😀");
            var other = new ZLinkStoreKey("ê");
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(exact, new byte[] {1}, null),
                                            new ZLinkStorePut(suffix, new byte[] {2}, null),
                                            new ZLinkStorePut(other, new byte[] {3}, null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(new ZLinkStoreScanRequest("é", null, 1), () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertEquals(List.of(exact), first.items().stream().map(item -> item.key()).toList());
            assertTrue(first.nextCursor() != null);
            var second =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "é", first.nextCursor(), 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertEquals(List.of(suffix), second.items().stream().map(item -> item.key()).toList());
            assertNull(second.nextCursor());
            var all =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(new ZLinkStoreScanRequest("", null, 3), () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertEquals(
                    List.of(exact, suffix, other),
                    all.items().stream().map(item -> item.key()).toList());
            assertNull(all.nextCursor());
        }
    }

    @Test
    void prefixScanDoesNotPageThroughUnrelatedRecords() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("prefix-scan:" + UUID.randomUUID()))) {
            var mutations =
                    new ArrayList<systems.zlink.framework.locationprovider.ZLinkStoreMutation>();
            for (int index = 0; index < 192; index++) {
                mutations.add(
                        new ZLinkStorePut(
                                new ZLinkStoreKey("authority/" + index), new byte[] {1}, null));
                mutations.add(
                        new ZLinkStorePut(
                                new ZLinkStoreKey("terminal/" + index), new byte[] {2}, null));
            }
            var target = new ZLinkStoreKey("descriptor/mesh");
            mutations.add(new ZLinkStorePut(target, new byte[] {3}, null));
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    store.write(new ZLinkStoreWriteRequest(List.of(), mutations), () -> false)
                            .toCompletableFuture()
                            .get());

            var page =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "descriptor/", null, 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();

            assertEquals(List.of(target), page.items().stream().map(item -> item.key()).toList());
            assertNull(page.nextCursor());
            var absent =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest("missing/", null, 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertTrue(absent.items().isEmpty());
            assertNull(absent.nextCursor());
        }
    }

    @Test
    void valueConditionChecksBytesAndExpiryAtomically() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("value:" + UUID.randomUUID()))) {
            var key = new ZLinkStoreKey("lease");
            var marker = new ZLinkStoreKey("marker");
            byte[] bytes = {0, (byte) 0xff};
            var put = new ZLinkStorePut(key, bytes, Duration.ofMinutes(1));
            var first =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(List.of(), List.of(put)),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(
                                            new ZLinkStoreVersionCondition(
                                                    key, first.putVersions().get(key))),
                                    List.of(put)),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            var condition = new ZLinkStoreValueCondition(key, bytes);
            var markerWrite =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(condition),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    marker, new byte[] {1}, null))),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(new ZLinkStorePut(key, new byte[] {2}, null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(), List.of(new ZLinkStoreDelete(key))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(marker, () -> false).toCompletableFuture().get());
            assertEquals(
                    markerWrite.putVersions().get(marker),
                    ((ZLinkStoreReadFound)
                                    store.read(marker, () -> false).toCompletableFuture().get())
                            .value()
                            .version());

            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(new ZLinkStorePut(key, bytes, Duration.ofMillis(1)))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            boolean expired = false;
            for (int attempt = 0; attempt < 1000; attempt++) {
                if (store.read(key, () -> false).toCompletableFuture().get()
                        instanceof ZLinkStoreReadMissing) {
                    expired = true;
                    break;
                }
            }
            assertTrue(expired);
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(marker, () -> false).toCompletableFuture().get());
        }
    }

    @Test
    void publicStoreImplementsOpaqueProviderContract() {
        assertTrue(ZLinkLocationStore.class.isAssignableFrom(ZLinkRedisLocationStore.class));
    }

    @Test
    void validatesProviderBoundsBeforeConnecting() {
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString("redis://127.0.0.1:1")
                                .setKeyPrefix("validation:" + UUID.randomUUID()))) {
            assertThrows(
                    IllegalArgumentException.class,
                    () -> store.read(new ZLinkStoreKey(""), () -> false));
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            new ZLinkStoreKey("large"),
                                                            new byte[1024 * 1024 + 1],
                                                            null))),
                                    () -> false));
            assertThrows(
                    CancellationException.class,
                    () ->
                            store.read(new ZLinkStoreKey("cancelled"), () -> true)
                                    .toCompletableFuture()
                                    .join());
        }
    }

    @Test
    void conditionalBatchAndScanUseStableOpaqueSnapshot() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("opaque:" + UUID.randomUUID()))) {
            var keyA = new ZLinkStoreKey("descriptor/a");
            var keyB = new ZLinkStoreKey("descriptor/b");
            var initial =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreMissingCondition(keyA),
                                                            new ZLinkStoreMissingCondition(keyB)),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    keyA,
                                                                    new byte[] {1},
                                                                    Duration.ofMinutes(5)),
                                                            new ZLinkStorePut(
                                                                    keyB, new byte[] {2}, null))),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());

            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(new ZLinkStoreMissingCondition(keyA)),
                                            List.of(new ZLinkStoreDelete(keyB))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            assertArrayEquals(
                    new byte[] {2},
                    assertInstanceOf(
                                    ZLinkStoreReadFound.class,
                                    store.read(keyB, () -> false).toCompletableFuture().get())
                            .value()
                            .bytes());

            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "descriptor/", null, 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertArrayEquals(new byte[] {1}, first.items().getFirst().value().bytes());

            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(
                                            new ZLinkStoreVersionCondition(
                                                    keyB, initial.putVersions().get(keyB))),
                                    List.of(new ZLinkStorePut(keyB, new byte[] {9}, null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();

            var second =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "descriptor/", first.nextCursor(), 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertArrayEquals(new byte[] {2}, second.items().getFirst().value().bytes());
            assertArrayEquals(
                    new byte[] {9},
                    assertInstanceOf(
                                    ZLinkStoreReadFound.class,
                                    store.read(keyB, () -> false).toCompletableFuture().get())
                            .value()
                            .bytes());
        }
    }

    @Test
    void multiKeyCasKeepsEachConditionBoundToItsKey() throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("condition-order:" + UUID.randomUUID()))) {
            // These names intentionally iterate in the opposite order in a
            // HashSet. The provider must preserve request order because the
            // Lua script binds condition i to KEYS[i].
            var existingKey = new ZLinkStoreKey("z");
            var missingKey = new ZLinkStoreKey("a");
            var initial =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreMissingCondition(
                                                                    existingKey)),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    existingKey,
                                                                    new byte[] {1},
                                                                    null))),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());

            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(
                                                    new ZLinkStoreVersionCondition(
                                                            existingKey,
                                                            initial.putVersions().get(existingKey)),
                                                    new ZLinkStoreMissingCondition(missingKey)),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            missingKey, new byte[] {2}, null))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());

            assertArrayEquals(
                    new byte[] {2},
                    assertInstanceOf(
                                    ZLinkStoreReadFound.class,
                                    store.read(missingKey, () -> false).toCompletableFuture().get())
                            .value()
                            .bytes());
        }
    }
}
