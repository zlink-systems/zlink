package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.core.json.JsonWriteFeature;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMutation;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPage;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.Objects;
import java.util.TreeMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Stores and validates immutable Location inventory records. */
final class ZLinkAggregateInventoryStore {
    static final int MAX_PAGE_ENTRIES = 1_024;
    static final int MAX_PAGE_BYTES = 1 << 20;
    private static final int MAX_TREE_LEVELS = 32;
    private static final String PREFIX = "zlink:v11:aggregate-inventory:";
    private static final HexFormat HEX = HexFormat.of();
    private static final ObjectMapper JSON =
            new ObjectMapper(
                            JsonFactory.builder()
                                    .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
                                    .disable(JsonWriteFeature.WRITE_HEX_UPPER_CASE)
                                    .build())
                    .enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);
    private static final java.util.regex.Pattern SHA256 =
            java.util.regex.Pattern.compile("[0-9a-f]{64}");
    private static final java.util.regex.Pattern AUTHORITY_KEY_PREFIX =
            java.util.regex.Pattern.compile("zla1:[as]:[1-9][0-9]{0,2}:");
    private final ZLinkLocationStore provider;

    ZLinkAggregateInventoryStore(ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    CompletionStage<Void> store(
            ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) {
        Tree tree = tree(entries(request.participants()));
        if (!Arrays.equals(tree.digest(), request.inventoryDigest())) {
            return failed(dataLost("aggregate inventory differs from the prepare request"));
        }
        var fence = new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration());
        return provider.read(rootKey(fence), cancellation)
                .thenCompose(
                        read -> {
                            if (read instanceof ZLinkStoreReadFound found) {
                                return Arrays.equals(found.value().bytes(), canonical(tree.root()))
                                        ? completed(null)
                                        : failed(dataLost("immutable inventory root changed"));
                            }
                            return store(fence, request.participants(), tree, cancellation)
                                    .thenApply(ignored -> null);
                        });
    }

    CompletionStage<byte[]> store(
            ZLinkAggregateFence fence,
            List<ZLinkAggregateParticipant> participants,
            ZLinkStoreCancellation cancellation) {
        return store(fence, participants, tree(entries(participants)), cancellation);
    }

    private CompletionStage<byte[]> store(
            ZLinkAggregateFence fence,
            List<ZLinkAggregateParticipant> participants,
            Tree tree,
            ZLinkStoreCancellation cancellation) {
        CompletionStage<Void> writes = completed(null);
        for (Page page : tree.pages()) {
            writes =
                    writes.thenCompose(
                            ignored ->
                                    putImmutable(
                                            pageKey(
                                                    fence,
                                                    page.reference().level(),
                                                    page.reference().index()),
                                            page.bytes(),
                                            cancellation));
        }
        for (int index = 0; index < participants.size(); index++) {
            var participant = participants.get(index);
            var payloadKey = participantPayloadKey(fence, index);
            var membershipKey = participantMembershipKey(fence, index);
            writes =
                    writes.thenCompose(
                                    ignored ->
                                            putImmutable(
                                                    payloadKey,
                                                    participant.authorityPayload(),
                                                    cancellation))
                            .thenCompose(
                                    ignored ->
                                            putImmutable(
                                                    membershipKey,
                                                    participant.membershipMutation(),
                                                    cancellation));
        }
        return writes.thenCompose(
                        ignored ->
                                putImmutable(rootKey(fence), canonical(tree.root()), cancellation))
                .thenApply(ignored -> tree.digest());
    }

    static byte[] digest(List<ZLinkAggregateParticipant> participants) {
        return tree(entries(participants)).digest();
    }

    private static List<ObjectNode> entries(List<ZLinkAggregateParticipant> participants) {
        List<ObjectNode> entries = new ArrayList<>(participants.size());
        for (var participant : participants) {
            byte[] payload = participant.authorityPayload();
            byte[] membership = participant.membershipMutation();
            if (!validBodies(payload, membership)) {
                throw new IllegalArgumentException("aggregate participant body size is invalid");
            }
            ObjectNode entry =
                    JSON.createObjectNode()
                            .put("index", entries.size())
                            .put("authorityKey", participant.authorityKey())
                            .put("expectedStoreVersion", participant.expectedStoreVersion())
                            .put("ownerTransition", transition(participant.ownerTransition()))
                            .put("authorityPayloadSha256", hash(payload))
                            .put("membershipMutationSha256", hash(membership));
            validateEntry(entry, entries);
            entries.add(entry);
        }
        return entries;
    }

    CompletionStage<List<StoredParticipant>> load(
            ZLinkAggregateFence fence,
            int expectedCount,
            byte[] digest,
            ZLinkStoreCancellation cancellation) {
        return readValue(rootKey(fence), cancellation)
                .thenCompose(
                        bytes -> {
                            ObjectNode root =
                                    object(
                                            bytes,
                                            "digestSha256",
                                            "totalCount",
                                            "topLevel",
                                            "topPages");
                            int count = number(root, "totalCount");
                            int level = number(root, "topLevel");
                            ArrayNode pages = array(root, "topPages");
                            if ((expectedCount >= 0 && count != expectedCount)
                                    || level >= MAX_TREE_LEVELS
                                    || pages.size() != (count == 0 ? 0 : 1)
                                    || (count == 0 && level != 0)
                                    || !text(root, "digestSha256").equals(HEX.formatHex(digest))) {
                                throw dataLost("aggregate inventory root does not match source");
                            }
                            List<ObjectNode> entries = new ArrayList<>();
                            int[] pageIndices = new int[level + 1];
                            CompletionStage<Void> reads =
                                    pages.isEmpty()
                                            ? completed(null)
                                            : readPage(
                                                    fence,
                                                    reference(pages.get(0)),
                                                    level,
                                                    count,
                                                    pageIndices,
                                                    entries,
                                                    cancellation);
                            return reads.thenCompose(
                                    ignored -> {
                                        if (entries.size() != count
                                                || !Arrays.equals(tree(entries).digest(), digest)) {
                                            throw dataLost(
                                                    "aggregate inventory root checksum or partition is invalid");
                                        }
                                        return readParticipants(fence, entries, cancellation);
                                    });
                        });
    }

    private CompletionStage<Void> readPage(
            ZLinkAggregateFence fence,
            Reference reference,
            int level,
            int count,
            int[] pageIndices,
            List<ObjectNode> entries,
            ZLinkStoreCancellation cancellation) {
        if (reference.level() != level
                || reference.index() != pageIndices[level]++
                || reference.startIndex() != entries.size()
                || reference.entryCount() != count
                || count < 1) {
            throw dataLost("aggregate inventory pages are out of order");
        }
        return readValue(pageKey(fence, level, reference.index()), cancellation)
                .thenCompose(
                        bytes -> {
                            if (bytes.length > MAX_PAGE_BYTES
                                    || !reference.sha256().equals(hash(bytes))) {
                                throw dataLost("aggregate inventory page checksum is invalid");
                            }
                            ObjectNode page =
                                    object(
                                            bytes,
                                            "level",
                                            "index",
                                            "startIndex",
                                            "entryCount",
                                            "entries",
                                            "children");
                            if (number(page, "level") != level
                                    || number(page, "index") != reference.index()
                                    || number(page, "startIndex") != reference.startIndex()
                                    || number(page, "entryCount") != count) {
                                throw dataLost(
                                        "aggregate inventory page metadata differs from reference");
                            }
                            ArrayNode leaf = array(page, "entries");
                            ArrayNode children = array(page, "children");
                            if (level == 0) {
                                if (!children.isEmpty() || leaf.size() != count) {
                                    throw dataLost("aggregate inventory leaf count is invalid");
                                }
                                for (var node : leaf) {
                                    ObjectNode entry =
                                            object(
                                                    node,
                                                    "index",
                                                    "authorityKey",
                                                    "expectedStoreVersion",
                                                    "ownerTransition",
                                                    "authorityPayloadSha256",
                                                    "membershipMutationSha256");
                                    validateEntry(entry, entries);
                                    entries.add(entry);
                                }
                                return completed(null);
                            }
                            if (!leaf.isEmpty() || children.isEmpty()) {
                                throw dataLost("aggregate inventory index page is invalid");
                            }
                            CompletionStage<Void> childrenRead = completed(null);
                            int remaining = count;
                            for (var node : children) {
                                Reference child = reference(node);
                                if (child.entryCount() > remaining) {
                                    throw dataLost("aggregate inventory child count is invalid");
                                }
                                remaining -= child.entryCount();
                                childrenRead =
                                        childrenRead.thenCompose(
                                                ignored ->
                                                        readPage(
                                                                fence,
                                                                child,
                                                                level - 1,
                                                                child.entryCount(),
                                                                pageIndices,
                                                                entries,
                                                                cancellation));
                            }
                            if (remaining != 0) {
                                throw dataLost("aggregate inventory index count is invalid");
                            }
                            return childrenRead;
                        });
    }

    private static void validateEntry(ObjectNode entry, List<ObjectNode> entries) {
        String key = text(entry, "authorityKey");
        String version = text(entry, "expectedStoreVersion");
        if (number(entry, "index") != entries.size()
                || !canonicalAuthorityKey(key)
                || version.isEmpty()
                || utf8(version).length > 4096
                || (!entries.isEmpty()
                        && compareKeys(text(entries.getLast(), "authorityKey"), key) >= 0)) {
            throw dataLost("aggregate inventory entries are invalid or out of order");
        }
        transition(text(entry, "ownerTransition"));
        checksum(entry, "authorityPayloadSha256");
        checksum(entry, "membershipMutationSha256");
    }

    private static boolean canonicalAuthorityKey(String key) {
        if (utf8(key).length > 776 || !AUTHORITY_KEY_PREFIX.matcher(key).lookingAt()) {
            return false;
        }
        try {
            var identity =
                    systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec.decode(key);
            return key.equals(
                    identity.kind().equals("actor")
                            ? systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec
                                    .actor(identity.id())
                            : systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec.spot(
                                    identity.id()));
        } catch (IllegalArgumentException invalid) {
            return false;
        }
    }

    private CompletionStage<List<StoredParticipant>> readParticipants(
            ZLinkAggregateFence fence,
            List<ObjectNode> entries,
            ZLinkStoreCancellation cancellation) {
        List<StoredParticipant> participants = new ArrayList<>(entries.size());
        CompletionStage<Void> reads = completed(null);
        for (var entry : entries) {
            int index = number(entry, "index");
            reads =
                    reads.thenCompose(
                            ignored ->
                                    readValue(participantPayloadKey(fence, index), cancellation)
                                            .thenCompose(
                                                    payload ->
                                                            readValue(
                                                                            participantMembershipKey(
                                                                                    fence, index),
                                                                            cancellation)
                                                                    .thenAccept(
                                                                            membership -> {
                                                                                if (!validBodies(
                                                                                                payload,
                                                                                                membership)
                                                                                        || !hash(payload)
                                                                                                .equals(
                                                                                                        text(
                                                                                                                entry,
                                                                                                                "authorityPayloadSha256"))
                                                                                        || !hash(membership)
                                                                                                .equals(
                                                                                                        text(
                                                                                                                entry,
                                                                                                                "membershipMutationSha256"))) {
                                                                                    throw dataLost(
                                                                                            "aggregate participant body is invalid");
                                                                                }
                                                                                participants.add(
                                                                                        new StoredParticipant(
                                                                                                text(
                                                                                                        entry,
                                                                                                        "authorityKey"),
                                                                                                text(
                                                                                                        entry,
                                                                                                        "expectedStoreVersion"),
                                                                                                transition(
                                                                                                        text(
                                                                                                                entry,
                                                                                                                "ownerTransition")),
                                                                                                payload,
                                                                                                membership));
                                                                            })));
        }
        return reads.thenApply(ignored -> List.copyOf(participants));
    }

    private static boolean validBodies(byte[] payload, byte[] membership) {
        return payload.length <= MAX_PAGE_BYTES
                && membership.length > 0
                && membership.length <= MAX_PAGE_BYTES;
    }

    /**
     * Deletes every immutable value belonging to one aggregate fence.
     *
     * <p>The aggregate marker remains as a terminal cleanup tombstone while this method runs. Every
     * item is also protected by the version observed in the scan page so a concurrent cleanup
     * cannot delete a replacement value.
     */
    CompletionStage<Void> delete(ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(cancellation, "cancellation");
        return delete(inventoryPrefix(fence), null, cancellation, 0);
    }

    private CompletionStage<Void> delete(
            String prefix,
            ZLinkStoreScanCursor cursor,
            ZLinkStoreCancellation cancellation,
            int restartCount) {
        return provider.scan(new ZLinkStoreScanRequest(prefix, cursor, 1_000), cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                if (restartCount >= 8) {
                                    return failed(
                                            new IllegalStateException(
                                                    "aggregate inventory cleanup scan repeatedly"
                                                            + " expired"));
                                }
                                return delete(prefix, null, cancellation, restartCount + 1);
                            }
                            ZLinkStoreScanPage page = ((ZLinkStoreScanPageResult) result).value();
                            List<ZLinkStoreCondition> conditions = new ArrayList<>();
                            List<ZLinkStoreMutation> mutations = new ArrayList<>();
                            for (var item : page.items()) {
                                conditions.add(
                                        new ZLinkStoreVersionCondition(
                                                item.key(), item.value().version()));
                                mutations.add(new ZLinkStoreDelete(item.key()));
                            }
                            CompletionStage<Void> deleted =
                                    mutations.isEmpty()
                                            ? completed(null)
                                            : provider.write(
                                                            new ZLinkStoreWriteRequest(
                                                                    conditions, mutations),
                                                            cancellation)
                                                    .thenCompose(
                                                            write -> {
                                                                if (write
                                                                        instanceof
                                                                        ZLinkStoreWriteApplied) {
                                                                    return completed(null);
                                                                }
                                                                if (restartCount >= 8) {
                                                                    return failed(
                                                                            new IllegalStateException(
                                                                                    "aggregate"
                                                                                            + " inventory"
                                                                                            + " cleanup"
                                                                                            + " repeatedly"
                                                                                            + " conflicted"));
                                                                }
                                                                return delete(
                                                                        prefix,
                                                                        null,
                                                                        cancellation,
                                                                        restartCount + 1);
                                                            });
                            return deleted.thenCompose(
                                    ignored ->
                                            page.nextCursor() == null
                                                    ? completed(null)
                                                    : delete(
                                                            prefix,
                                                            page.nextCursor(),
                                                            cancellation,
                                                            restartCount));
                        });
    }

    CompletionStage<byte[]> readParticipantPayload(
            ZLinkAggregateFence fence,
            int index,
            byte[] expectedHash,
            ZLinkStoreCancellation cancellation) {
        return readValue(participantPayloadKey(fence, index), cancellation)
                .thenApply(
                        payload -> {
                            if (payload.length > MAX_PAGE_BYTES
                                    || !Arrays.equals(expectedHash, sha256(payload))) {
                                throw dataLost("aggregate participant payload is invalid");
                            }
                            return payload;
                        });
    }

    private CompletionStage<byte[]> readValue(
            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
        return provider.read(key, cancellation)
                .thenCompose(
                        result ->
                                result instanceof ZLinkStoreReadFound found
                                        ? completed(found.value().bytes())
                                        : failed(
                                                new ZLinkAggregateRelocationCoordinator
                                                        .RelocationDataLostException(
                                                        "aggregate inventory value is missing: "
                                                                + key.value())));
    }

    private CompletionStage<Void> putImmutable(
            ZLinkStoreKey key, byte[] bytes, ZLinkStoreCancellation cancellation) {
        return provider.write(
                        new ZLinkStoreWriteRequest(
                                List.of(
                                        new systems.zlink.framework.locationprovider
                                                .ZLinkStoreMissingCondition(key)),
                                List.of(new ZLinkStorePut(key, bytes, null))),
                        cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreWriteApplied) {
                                return completed(null);
                            }
                            return readValue(key, cancellation)
                                    .thenCompose(
                                            existing ->
                                                    Arrays.equals(existing, bytes)
                                                            ? completed(null)
                                                            : failed(
                                                                    new IllegalStateException(
                                                                            "immutable aggregate"
                                                                                    + " inventory value"
                                                                                    + " changed: "
                                                                                    + key
                                                                                            .value())));
                        });
    }

    private static Tree tree(List<ObjectNode> entries) {
        List<Page> pages = new ArrayList<>();
        int level = 0;
        List<ObjectNode> top = pack(entries, level, pages);
        while (top.size() > 1) {
            top = pack(top, ++level, pages);
        }
        ObjectNode root =
                JSON.createObjectNode().put("totalCount", entries.size()).put("topLevel", level);
        root.putArray("topPages").addAll(top);
        byte[] digest = sha256(canonical(root));
        root.put("digestSha256", HEX.formatHex(digest));
        return new Tree(root, digest, pages);
    }

    private static List<ObjectNode> pack(List<ObjectNode> items, int level, List<Page> pages) {
        List<ObjectNode> references = new ArrayList<>();
        int startIndex = 0;
        int offset = 0;
        while (offset < items.size()) {
            int index = references.size();
            ObjectNode page =
                    JSON.createObjectNode()
                            .put("level", level)
                            .put("index", index)
                            .put("startIndex", startIndex)
                            .put("entryCount", 0);
            ArrayNode entries = page.putArray("entries");
            ArrayNode children = page.putArray("children");
            ArrayNode values = level == 0 ? entries : children;
            int overhead = canonical(page).length - 1;
            int elementBytes = 0;
            int count = 0;
            while (offset < items.size() && values.size() < MAX_PAGE_ENTRIES) {
                ObjectNode item = items.get(offset);
                int nextCount = count + (level == 0 ? 1 : number(item, "entryCount"));
                int nextBytes = elementBytes + canonical(item).length + (values.isEmpty() ? 0 : 1);
                if (overhead + Integer.toString(nextCount).length() + nextBytes > MAX_PAGE_BYTES) {
                    if (values.isEmpty()) {
                        throw new IllegalArgumentException(
                                "aggregate inventory item exceeds 1 MiB");
                    }
                    break;
                }
                values.add(item);
                elementBytes = nextBytes;
                count = nextCount;
                offset++;
            }
            page.put("entryCount", count);
            byte[] bytes = canonical(page);
            var ref = new Reference(level, index, startIndex, count, hash(bytes));
            pages.add(new Page(ref, bytes));
            references.add(
                    JSON.createObjectNode()
                            .put("level", level)
                            .put("index", index)
                            .put("startIndex", startIndex)
                            .put("entryCount", count)
                            .put("sha256", ref.sha256()));
            startIndex += count;
        }
        return references;
    }

    static byte[] canonical(JsonNode value) {
        try {
            return utf8(JSON.writeValueAsString(sorted(value)));
        } catch (IOException failure) {
            throw dataLost("cannot encode canonical aggregate JSON");
        }
    }

    // The storage schema uses only nonnegative signed 32-bit JSON numbers.
    private static JsonNode sorted(JsonNode value) {
        if (value.isObject()) {
            var fields = new TreeMap<String, JsonNode>();
            value.fields()
                    .forEachRemaining(
                            field -> fields.put(field.getKey(), sorted(field.getValue())));
            return JSON.createObjectNode().setAll(fields);
        }
        if (value.isArray()) {
            ArrayNode result = JSON.createArrayNode();
            value.forEach(item -> result.add(sorted(item)));
            return result;
        }
        if (value.isTextual()
                && !StandardCharsets.UTF_8.newEncoder().canEncode(value.textValue())) {
            throw dataLost("invalid Unicode in aggregate JSON string");
        }
        return value;
    }

    static ObjectNode object(byte[] bytes, String... fields) {
        try {
            if (bytes.length > MAX_PAGE_BYTES) {
                throw dataLost("aggregate JSON exceeds 1 MiB");
            }
            ObjectNode value = object(JSON.readTree(bytes), fields);
            if (!Arrays.equals(bytes, canonical(value))) {
                throw dataLost("noncanonical aggregate JSON");
            }
            return value;
        } catch (IOException failure) {
            throw dataLost("invalid aggregate JSON");
        }
    }

    static ObjectNode object(JsonNode value, String... fields) {
        if (!(value instanceof ObjectNode record) || record.size() != fields.length) {
            throw dataLost("invalid aggregate record fields");
        }
        for (String field : fields) {
            if (!record.has(field)) {
                throw dataLost("missing aggregate record field " + field);
            }
        }
        return record;
    }

    private static Reference reference(JsonNode node) {
        ObjectNode value = object(node, "level", "index", "startIndex", "entryCount", "sha256");
        return new Reference(
                number(value, "level"),
                number(value, "index"),
                number(value, "startIndex"),
                number(value, "entryCount"),
                checksum(value, "sha256"));
    }

    static String checksum(JsonNode value, String field) {
        String result = text(value, field);
        if (!SHA256.matcher(result).matches()) {
            throw dataLost("invalid aggregate checksum");
        }
        return result;
    }

    static String text(JsonNode value, String field) {
        if (!value.path(field).isTextual()) {
            throw dataLost("invalid aggregate field " + field);
        }
        return value.get(field).textValue();
    }

    static int number(JsonNode value, String field) {
        var number = value.path(field);
        if (!number.isIntegralNumber() || !number.canConvertToInt() || number.intValue() < 0) {
            throw dataLost("invalid aggregate field " + field);
        }
        return number.intValue();
    }

    private static ArrayNode array(JsonNode value, String field) {
        if (!(value.path(field) instanceof ArrayNode array) || array.size() > MAX_PAGE_ENTRIES) {
            throw dataLost("invalid aggregate field " + field);
        }
        return array;
    }

    private static String transition(ZLinkAuthorityGenerationTransition transition) {
        return transition == ZLinkAuthorityGenerationTransition.PRESERVE ? "preserve" : "newOwner";
    }

    private static ZLinkAuthorityGenerationTransition transition(String transition) {
        return switch (transition) {
            case "preserve" -> ZLinkAuthorityGenerationTransition.PRESERVE;
            case "newOwner" -> ZLinkAuthorityGenerationTransition.NEW_OWNER;
            default -> throw dataLost("invalid aggregate owner transition");
        };
    }

    private static byte[] utf8(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    static int compareKeys(String left, String right) {
        return Arrays.compareUnsigned(utf8(left), utf8(right));
    }

    static byte[] sha256(byte[] bytes) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(bytes);
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static String hash(byte[] bytes) {
        return HEX.formatHex(sha256(bytes));
    }

    static ZLinkStoreKey rootKey(ZLinkAggregateFence fence) {
        return new ZLinkStoreKey(inventoryPrefix(fence) + "root");
    }

    private static ZLinkStoreKey pageKey(ZLinkAggregateFence fence, int level, int index) {
        return new ZLinkStoreKey(inventoryPrefix(fence) + "page:" + level + ":" + index);
    }

    private static String inventoryPrefix(ZLinkAggregateFence fence) {
        return PREFIX + fence.aggregateId() + ":" + fence.aggregateGeneration() + ":";
    }

    private static ZLinkStoreKey participantPayloadKey(ZLinkAggregateFence fence, int index) {
        return new ZLinkStoreKey(
                inventoryPrefix(fence) + "participant:" + index + ":authorityPayload");
    }

    private static ZLinkStoreKey participantMembershipKey(ZLinkAggregateFence fence, int index) {
        return new ZLinkStoreKey(
                inventoryPrefix(fence) + "participant:" + index + ":membershipMutation");
    }

    private static ZLinkAggregateRelocationCoordinator.RelocationDataLostException dataLost(
            String message) {
        return new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(message);
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    record StoredParticipant(
            String authorityKey,
            String expectedStoreVersion,
            ZLinkAuthorityGenerationTransition ownerTransition,
            byte[] authorityPayload,
            byte[] membershipMutation) {}

    private record Tree(ObjectNode root, byte[] digest, List<Page> pages) {}

    private record Page(Reference reference, byte[] bytes) {}

    private record Reference(int level, int index, int startIndex, int entryCount, String sha256) {}
}
