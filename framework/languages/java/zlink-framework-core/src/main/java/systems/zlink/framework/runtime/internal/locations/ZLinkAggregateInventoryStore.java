package systems.zlink.framework.runtime.internal.locations;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPage;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreMutation;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;

/**
 * Stores an aggregate participant inventory as immutable pages.
 *
 * <p>The aggregate authority record contains only the inventory metadata. The
 * participant payload and membership mutation are stored under immutable
 * participant keys, so the provider batch used for the authority CAS remains
 * bounded even when an aggregate contains many participants.</p>
 */
final class ZLinkAggregateInventoryStore {
    static final int MAX_PAGE_ENTRIES = 1_024;
    static final int MAX_PAGE_BYTES = 1 * 1024 * 1024;
    static final int MAX_TREE_LEVELS = 32;
    private static final String PREFIX = "zlink:v11:aggregate-inventory:";
    private static final byte[] PAGE_MAGIC = {
        'Z', 'L', 'A', 'P'
    };
    private static final byte[] ROOT_MAGIC = {
        'Z', 'L', 'A', 'R'
    };
    private static final int HASH_BYTES = 32;
    private final ZLinkLocationStore provider;

    ZLinkAggregateInventoryStore(ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    CompletionStage<Void> store(
        ZLinkAggregatePrepareRequest request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        Tree tree = buildTree(request);
        ZLinkAggregateFence fence = fence(request);
        CompletionStage<Void> writes = CompletableFuture.completedFuture(null);
        for (Page page : tree.pages()) {
            byte[] bytes = encodePage(page);
            if (bytes.length > MAX_PAGE_BYTES) {
                return failed(new IllegalStateException(
                    "aggregate inventory page exceeds 1 MiB"));
            }
            writes = writes.thenCompose(ignored -> putImmutable(
                pageKey(fence, page.level(), page.index()),
                bytes,
                cancellation));
        }
        for (int index = 0; index < request.participants().size(); index++) {
            int participantIndex = index;
            ZLinkAggregateParticipant participant =
                request.participants().get(index);
            writes = writes
                .thenCompose(ignored -> putImmutable(
                    participantPayloadKey(fence, participantIndex),
                    participant.authorityPayload(),
                    cancellation))
                .thenCompose(ignored -> putImmutable(
                    participantMembershipKey(fence, participantIndex),
                    participant.membershipMutation(),
                    cancellation));
        }
        return writes
            .thenCompose(ignored -> putImmutable(
                rootKey(fence),
                encodeRoot(tree.root()),
                cancellation))
            .thenCompose(ignored -> load(
                fence,
                request.participants().size(),
                request.inventoryDigest(),
                cancellation))
            .thenApply(ignored -> null);
    }

    CompletionStage<List<ZLinkAggregateParticipant>> load(
        ZLinkAggregateFence fence,
        int expectedCount,
        byte[] declaredDigest,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(declaredDigest, "declaredDigest");
        Objects.requireNonNull(cancellation, "cancellation");
        if (expectedCount < 1 || declaredDigest.length != HASH_BYTES) {
            return failed(new IllegalArgumentException(
                "aggregate inventory metadata is invalid"));
        }
        return readValue(rootKey(fence), cancellation).thenCompose(bytes -> {
            Root root = decodeRoot(bytes);
            validateRoot(root, fence, expectedCount, declaredDigest);
            List<Entry> entries = new ArrayList<>(root.totalCount());
            Set<String> visited = new HashSet<>();
            int[] observedPageCounts = new int[root.topLevel() + 1];
            CompletionStage<Void> pages = CompletableFuture.completedFuture(null);
            int expectedStart = 0;
            for (Reference reference : root.topPages()) {
                if (reference.level() != root.topLevel()
                    || reference.startIndex() != expectedStart) {
                    return failed(dataLost(
                        fence,
                        "aggregate inventory top pages are reordered"));
                }
                expectedStart = Math.addExact(
                    expectedStart,
                    reference.entryCount());
                pages = pages.thenCompose(ignored -> readPage(
                    fence,
                    root,
                    reference,
                    entries,
                    visited,
                    observedPageCounts,
                    cancellation));
            }
            if (expectedStart != root.totalCount()) {
                return failed(dataLost(
                    fence,
                    "aggregate inventory root count is invalid"));
            }
            return pages.thenCompose(ignored -> {
                validateObservedPages(root, entries, visited, observedPageCounts,
                    fence);
                return loadParticipantValues(
                    fence,
                    entries,
                    cancellation);
            });
        });
    }

    /**
     * Deletes every immutable value belonging to one aggregate fence.
     *
     * <p>The aggregate marker is removed by the authority repository before
     * this method is called. Every item is still protected by the version
     * observed in the scan page so a concurrent cleanup cannot delete a
     * replacement value.</p>
     */
    CompletionStage<Void> delete(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(cancellation, "cancellation");
        return delete(inventoryPrefix(fence), null, cancellation, 0);
    }

    private CompletionStage<Void> delete(
        String prefix,
        ZLinkStoreScanCursor cursor,
        ZLinkStoreCancellation cancellation,
        int restartCount) {
        return provider.scan(
                new ZLinkStoreScanRequest(prefix, cursor, 1_000),
                cancellation)
            .thenCompose(result -> {
                if (result instanceof ZLinkStoreScanExpired) {
                    if (restartCount >= 8) {
                        return failed(new IllegalStateException(
                            "aggregate inventory cleanup scan repeatedly expired"));
                    }
                    return delete(prefix, null, cancellation, restartCount + 1);
                }
                ZLinkStoreScanPage page =
                    ((ZLinkStoreScanPageResult) result).value();
                List<ZLinkStoreCondition> conditions = new ArrayList<>();
                List<ZLinkStoreMutation> mutations = new ArrayList<>();
                for (var item : page.items()) {
                    conditions.add(new ZLinkStoreVersionCondition(
                        item.key(),
                        item.value().version()));
                    mutations.add(new ZLinkStoreDelete(item.key()));
                }
                CompletionStage<Void> deleted = mutations.isEmpty()
                    ? completed(null)
                    : provider.write(
                            new ZLinkStoreWriteRequest(
                                conditions,
                                mutations),
                            cancellation)
                        .thenCompose(write -> {
                            if (write instanceof ZLinkStoreWriteApplied) {
                                return completed(null);
                            }
                            if (restartCount >= 8) {
                                return failed(new IllegalStateException(
                                    "aggregate inventory cleanup repeatedly conflicted"));
                            }
                            return delete(
                                prefix,
                                null,
                                cancellation,
                                restartCount + 1);
                        });
                return deleted.thenCompose(ignored ->
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
        ZLinkStoreCancellation cancellation) {
        if (index < 0) {
            return failed(new IllegalArgumentException(
                "aggregate participant index must not be negative"));
        }
        return readValue(participantPayloadKey(fence, index), cancellation);
    }

    static byte[] fingerprint(ZLinkAggregatePrepareRequest request) {
        Objects.requireNonNull(request, "request");
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            out.writeLong(request.aggregateId().getMostSignificantBits());
            out.writeLong(request.aggregateId().getLeastSignificantBits());
            out.writeLong(request.aggregateGeneration());
            writeString(out, request.targetDescriptor().meshName());
            writeString(out, request.targetDescriptor().rid().toHex());
            out.writeLong(request.targetDescriptorLifecycleGeneration());
            writeString(out, request.targetOwner().ownerId());
            out.writeLong(request.targetOwner().leaseGeneration());
            writeBytes(out, request.inventoryDigest());
            out.writeInt(request.capacityBundle().actorSlots());
            out.writeInt(request.capacityBundle().spotSlots());
            out.writeBoolean(request.capacityBundle().spotType().isPresent());
            if (request.capacityBundle().spotType().isPresent()) {
                var delta = request.capacityBundle().spotType().orElseThrow();
                out.writeInt(delta.objectKind().ordinal());
                writeString(out, delta.stableType());
                out.writeInt(delta.slots());
            }
            out.writeInt(request.participants().size());
            for (ZLinkAggregateParticipant participant
                : request.participants()) {
                writeString(out, participant.authorityKey());
                out.writeLong(participant.objectGeneration());
                out.writeLong(participant.sourceAuthorityOwnerGeneration());
                writeString(out, participant.expectedStoreVersion());
                out.writeInt(participant.ownerTransition().ordinal());
                writeBytes(out, participant.authorityPayload());
                writeBytes(out, participant.membershipMutation());
            }
            out.flush();
            return sha256(bytes.toByteArray());
        } catch (IOException failure) {
            throw new IllegalStateException(
                "aggregate request fingerprint could not be encoded",
                failure);
        }
    }

    static ZLinkStoreKey rootKey(ZLinkAggregateFence fence) {
        return new ZLinkStoreKey(PREFIX + fence.aggregateId()
            + ":" + fence.aggregateGeneration() + ":root");
    }

    private CompletionStage<Void> readPage(
        ZLinkAggregateFence fence,
        Root root,
        Reference reference,
        List<Entry> entries,
        Set<String> visited,
        int[] observedPageCounts,
        ZLinkStoreCancellation cancellation) {
        validateReference(reference, root, fence);
        String identity = reference.level() + ":" + reference.index();
        if (!visited.add(identity)) {
            return failed(dataLost(
                fence,
                "aggregate inventory page is referenced more than once"));
        }
        return readValue(
                pageKey(fence, reference.level(), reference.index()),
                cancellation)
            .thenCompose(bytes -> {
                if (bytes.length > MAX_PAGE_BYTES
                    || !Arrays.equals(reference.sha256(), sha256(bytes))) {
                    return failed(dataLost(
                        fence,
                        "aggregate inventory page checksum is invalid"));
                }
                Page page = decodePage(bytes);
                if (page.level() != reference.level()
                    || page.index() != reference.index()
                    || page.startIndex() != reference.startIndex()
                    || page.entryCount() != reference.entryCount()) {
                    return failed(dataLost(
                        fence,
                        "aggregate inventory page metadata changed"));
                }
                observedPageCounts[page.level()]++;
                if (page.level() == 0) {
                    if (page.entries().isEmpty()
                        || !page.children().isEmpty()
                        || page.entries().size() != page.entryCount()) {
                        return failed(dataLost(
                            fence,
                            "aggregate inventory leaf bounds are invalid"));
                    }
                    for (Entry entry : page.entries()) {
                        if (entry.index() != entries.size()) {
                            return failed(dataLost(
                                fence,
                                "aggregate inventory entries are reordered"));
                        }
                        entries.add(entry);
                    }
                    return completed(null);
                }
                if (!page.entries().isEmpty() || page.children().isEmpty()) {
                    return failed(dataLost(
                        fence,
                        "aggregate inventory index bounds are invalid"));
                }
                CompletionStage<Void> children =
                    CompletableFuture.completedFuture(null);
                int childStart = page.startIndex();
                for (Reference child : page.children()) {
                    if (child.level() != page.level() - 1
                        || child.startIndex() != childStart) {
                        return failed(dataLost(
                            fence,
                            "aggregate inventory index children are reordered"));
                    }
                    childStart = Math.addExact(
                        childStart,
                        child.entryCount());
                    children = children.thenCompose(ignored -> readPage(
                        fence,
                        root,
                        child,
                        entries,
                        visited,
                        observedPageCounts,
                        cancellation));
                }
                if (childStart != Math.addExact(
                        page.startIndex(),
                        page.entryCount())) {
                    return failed(dataLost(
                        fence,
                        "aggregate inventory index count is invalid"));
                }
                return children;
            });
    }

    private CompletionStage<List<ZLinkAggregateParticipant>>
        loadParticipantValues(
            ZLinkAggregateFence fence,
            List<Entry> entries,
            ZLinkStoreCancellation cancellation) {
        List<ZLinkAggregateParticipant> participants =
            new ArrayList<>(entries.size());
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (int index = 0; index < entries.size(); index++) {
            int participantIndex = index;
            Entry entry = entries.get(index);
            chain = chain.thenCompose(ignored ->
                readValue(
                        participantPayloadKey(fence, participantIndex),
                        cancellation)
                    .thenCompose(payload ->
                        readValue(
                                participantMembershipKey(fence, participantIndex),
                                cancellation)
                            .thenAccept(membership -> {
                                if (!Arrays.equals(
                                        entry.authorityPayloadSha256(),
                                        sha256(payload))
                                    || !Arrays.equals(
                                        entry.membershipMutationSha256(),
                                        sha256(membership))) {
                                    throw dataLost(
                                        fence,
                                        "aggregate participant checksum is invalid");
                                }
                                participants.add(new ZLinkAggregateParticipant(
                                    entry.authorityKey(),
                                    entry.objectGeneration(),
                                    entry.sourceAuthorityOwnerGeneration(),
                                    entry.expectedStoreVersion(),
                                    entry.ownerTransition(),
                                    payload,
                                    membership));
                            })));
        }
        return chain.thenApply(ignored -> List.copyOf(participants));
    }

    private CompletionStage<byte[]> readValue(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation) {
        return provider.read(key, cancellation).thenCompose(result ->
            result instanceof ZLinkStoreReadFound found
                ? completed(found.value().bytes())
                : failed(new IllegalStateException(
                    "aggregate inventory value is missing: " + key.value())));
    }

    private CompletionStage<Void> putImmutable(
        ZLinkStoreKey key,
        byte[] bytes,
        ZLinkStoreCancellation cancellation) {
        return provider.write(
                new ZLinkStoreWriteRequest(
                    List.of(new systems.zlink.framework.locationprovider
                        .ZLinkStoreMissingCondition(key)),
                    List.of(new ZLinkStorePut(key, bytes, null))),
                cancellation)
            .thenCompose(result -> {
                if (result instanceof ZLinkStoreWriteApplied) {
                    return completed(null);
                }
                return readValue(key, cancellation).thenCompose(existing ->
                    Arrays.equals(existing, bytes)
                        ? completed(null)
                        : failed(new IllegalStateException(
                            "immutable aggregate inventory value changed: "
                                + key.value())));
            });
    }

    private static Tree buildTree(ZLinkAggregatePrepareRequest request) {
        List<Entry> entries = new ArrayList<>(request.participants().size());
        for (int index = 0; index < request.participants().size(); index++) {
            ZLinkAggregateParticipant participant =
                request.participants().get(index);
            entries.add(new Entry(
                index,
                participant.authorityKey(),
                participant.objectGeneration(),
                participant.sourceAuthorityOwnerGeneration(),
                participant.expectedStoreVersion(),
                participant.ownerTransition(),
                sha256(participant.authorityPayload()),
                sha256(participant.membershipMutation())));
        }
        byte[] digest = digestEntries(entries);
        List<Page> pages = new ArrayList<>();
        List<Reference> references = packLeafPages(entries, pages);
        int level = 0;
        while (references.size() > MAX_PAGE_ENTRIES
            || encodeRoot(rootCandidate(
                entries.size(),
                level,
                references,
                pages,
                digest,
                request.inventoryDigest())).length > MAX_PAGE_BYTES) {
            level++;
            if (level >= MAX_TREE_LEVELS) {
                throw new IllegalArgumentException(
                    "aggregate inventory tree exceeds 32 levels");
            }
            references = packIndexPages(level, references, pages);
        }
        Root root = rootCandidate(
            entries.size(),
            level,
            references,
            pages,
            digest,
            request.inventoryDigest());
        if (encodeRoot(root).length > MAX_PAGE_BYTES) {
            throw new IllegalArgumentException(
                "aggregate inventory root exceeds 1 MiB");
        }
        return new Tree(root, List.copyOf(pages));
    }

    private static List<Reference> packLeafPages(
        List<Entry> entries,
        List<Page> pages) {
        List<Reference> references = new ArrayList<>();
        for (int offset = 0; offset < entries.size();) {
            int count = Math.min(MAX_PAGE_ENTRIES, entries.size() - offset);
            Page page;
            byte[] encoded;
            do {
                page = new Page(
                    0,
                    references.size(),
                    offset,
                    count,
                    List.copyOf(entries.subList(offset, offset + count)),
                    List.of());
                encoded = encodePage(page);
                if (encoded.length <= MAX_PAGE_BYTES) {
                    break;
                }
                count /= 2;
            } while (count > 0);
            if (count < 1 || encoded.length > MAX_PAGE_BYTES) {
                throw new IllegalArgumentException(
                    "aggregate inventory entry exceeds 1 MiB");
            }
            pages.add(page);
            references.add(referenceFor(page, encoded));
            offset += count;
        }
        return references;
    }

    private static List<Reference> packIndexPages(
        int level,
        List<Reference> children,
        List<Page> pages) {
        List<Reference> references = new ArrayList<>();
        for (int offset = 0; offset < children.size();) {
            int count = Math.min(MAX_PAGE_ENTRIES, children.size() - offset);
            Page page;
            byte[] encoded;
            do {
                List<Reference> selected = List.copyOf(
                    children.subList(offset, offset + count));
                int entryCount = selected.stream()
                    .mapToInt(Reference::entryCount)
                    .sum();
                page = new Page(
                    level,
                    references.size(),
                    selected.getFirst().startIndex(),
                    entryCount,
                    List.of(),
                    selected);
                encoded = encodePage(page);
                if (encoded.length <= MAX_PAGE_BYTES) {
                    break;
                }
                count /= 2;
            } while (count > 0);
            if (count < 1 || encoded.length > MAX_PAGE_BYTES) {
                throw new IllegalArgumentException(
                    "aggregate inventory page references exceed 1 MiB");
            }
            pages.add(page);
            references.add(referenceFor(page, encoded));
            offset += count;
        }
        return references;
    }

    private static Root rootCandidate(
        int totalCount,
        int topLevel,
        List<Reference> topPages,
        List<Page> pages,
        byte[] digest,
        byte[] declaredDigest) {
        int[] pageCounts = new int[topLevel + 1];
        for (Page page : pages) {
            pageCounts[page.level()]++;
        }
        return new Root(
            totalCount,
            digest,
            declaredDigest,
            topLevel,
            List.copyOf(topPages),
            pageCounts);
    }

    private static Reference referenceFor(Page page, byte[] encoded) {
        return new Reference(
            page.level(),
            page.index(),
            page.startIndex(),
            page.entryCount(),
            sha256(encoded));
    }

    private static void validateRoot(
        Root root,
        ZLinkAggregateFence fence,
        int expectedCount,
        byte[] declaredDigest) {
        if (root.totalCount() != expectedCount
            || root.totalCount() < 1
            || !Arrays.equals(root.declaredDigest(), declaredDigest)
            || root.topLevel() < 0
            || root.topLevel() >= MAX_TREE_LEVELS
            || root.topPages().isEmpty()
            || root.topPages().size() > MAX_PAGE_ENTRIES
            || root.pageCounts().length != root.topLevel() + 1) {
            throw dataLost(fence, "aggregate inventory root metadata is invalid");
        }
        int leafPages = root.pageCounts()[0];
        if (leafPages < 1
            || leafPages > root.totalCount()
            || (long) root.totalCount()
                > (long) leafPages * MAX_PAGE_ENTRIES
            || root.topPages().size()
                != root.pageCounts()[root.topLevel()]) {
            throw dataLost(fence, "aggregate inventory root page counts are invalid");
        }
        for (int level = 0; level < root.pageCounts().length; level++) {
            if (root.pageCounts()[level] < 1
                || root.pageCounts()[level] > root.totalCount()) {
                throw dataLost(fence, "aggregate inventory page count is invalid");
            }
            if (level > 0) {
                int lower = root.pageCounts()[level - 1];
                int current = root.pageCounts()[level];
                if (current < (int) (((long) lower
                        + MAX_PAGE_ENTRIES - 1)
                        / MAX_PAGE_ENTRIES)
                    || current > lower) {
                    throw dataLost(fence, "aggregate inventory tree level is invalid");
                }
            }
        }
    }

    private static void validateReference(
        Reference reference,
        Root root,
        ZLinkAggregateFence fence) {
        if (reference.level() < 0
            || reference.level() > root.topLevel()
            || reference.index() < 0
            || reference.startIndex() < 0
            || reference.entryCount() < 1
            || reference.index() >= root.pageCounts()[reference.level()]
            || (long) reference.startIndex() + reference.entryCount()
                > root.totalCount()
            || reference.sha256().length != HASH_BYTES) {
            throw dataLost(fence, "aggregate inventory page reference is invalid");
        }
    }

    private static void validateObservedPages(
        Root root,
        List<Entry> entries,
        Set<String> visited,
        int[] observedPageCounts,
        ZLinkAggregateFence fence) {
        validateEntries(entries, fence);
        if (entries.size() != root.totalCount()
            || !Arrays.equals(digestEntries(entries), root.digest())) {
            throw dataLost(fence, "aggregate inventory digest does not match");
        }
        for (int level = 0; level < observedPageCounts.length; level++) {
            if (observedPageCounts[level] != root.pageCounts()[level]) {
                throw dataLost(fence, "aggregate inventory page counts changed");
            }
            for (int index = 0; index < observedPageCounts[level]; index++) {
                if (!visited.contains(level + ":" + index)) {
                    throw dataLost(fence, "aggregate inventory page indexes are not contiguous");
                }
            }
        }
    }

    private static void validateEntries(
        List<Entry> entries,
        ZLinkAggregateFence fence) {
        byte[] previousKey = null;
        for (int index = 0; index < entries.size(); index++) {
            Entry entry = entries.get(index);
            byte[] currentKey = entry.authorityKey()
                .getBytes(StandardCharsets.UTF_8);
            if (entry.index() != index
                || currentKey.length == 0
                || entry.expectedStoreVersion().isBlank()
                || entry.objectGeneration() <= 0
                || entry.sourceAuthorityOwnerGeneration() <= 0
                || entry.authorityPayloadSha256().length != HASH_BYTES
                || entry.membershipMutationSha256().length != HASH_BYTES
                || previousKey != null
                    && Arrays.compareUnsigned(previousKey, currentKey) >= 0) {
                throw dataLost(
                    fence,
                    "aggregate inventory entries are invalid or reordered");
            }
            previousKey = currentKey;
        }
    }

    private static Page decodePage(byte[] bytes) {
        try {
            var in = input(bytes, PAGE_MAGIC);
            int level = in.readInt();
            int index = in.readInt();
            int start = in.readInt();
            int entryCount = in.readInt();
            int entrySize = in.readInt();
            int childSize = in.readInt();
            if (level < 0 || level >= MAX_TREE_LEVELS
                || index < 0 || start < 0 || entryCount < 1
                || entrySize < 0 || entrySize > MAX_PAGE_ENTRIES
                || childSize < 0 || childSize > MAX_PAGE_ENTRIES
                || entrySize > 0 && childSize > 0) {
                throw new IOException("invalid aggregate inventory page bounds");
            }
            List<Entry> entries = new ArrayList<>(entrySize);
            for (int i = 0; i < entrySize; i++) {
                entries.add(readEntry(in));
            }
            List<Reference> children = new ArrayList<>(childSize);
            for (int i = 0; i < childSize; i++) {
                children.add(readReference(in));
            }
            if (in.available() != 0) {
                throw new IOException("aggregate inventory page has trailing bytes");
            }
            return new Page(
                level,
                index,
                start,
                entryCount,
                entries,
                children);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException(
                "aggregate inventory page is invalid",
                failure);
        }
    }

    private static Root decodeRoot(byte[] bytes) {
        try {
            var in = input(bytes, ROOT_MAGIC);
            int totalCount = in.readInt();
            byte[] digest = in.readNBytes(HASH_BYTES);
            byte[] declaredDigest = in.readNBytes(HASH_BYTES);
            int topLevel = in.readInt();
            int topPageCount = in.readInt();
            if (digest.length != HASH_BYTES
                || declaredDigest.length != HASH_BYTES
                || totalCount < 1
                || topLevel < 0
                || topLevel >= MAX_TREE_LEVELS
                || topPageCount < 1
                || topPageCount > MAX_PAGE_ENTRIES) {
                throw new IOException("invalid aggregate inventory root bounds");
            }
            List<Reference> topPages = new ArrayList<>(topPageCount);
            for (int i = 0; i < topPageCount; i++) {
                topPages.add(readReference(in));
            }
            int pageCountSize = in.readInt();
            if (pageCountSize != topLevel + 1) {
                throw new IOException("invalid aggregate inventory level count");
            }
            int[] pageCounts = new int[pageCountSize];
            for (int i = 0; i < pageCounts.length; i++) {
                pageCounts[i] = in.readInt();
            }
            if (in.available() != 0) {
                throw new IOException("aggregate inventory root has trailing bytes");
            }
            return new Root(
                totalCount,
                digest,
                declaredDigest,
                topLevel,
                topPages,
                pageCounts);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException(
                "aggregate inventory root is invalid",
                failure);
        }
    }

    private static byte[] encodePage(Page page) {
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            out.write(PAGE_MAGIC);
            out.writeByte(1);
            out.writeInt(page.level());
            out.writeInt(page.index());
            out.writeInt(page.startIndex());
            out.writeInt(page.entryCount());
            out.writeInt(page.entries().size());
            out.writeInt(page.children().size());
            for (Entry entry : page.entries()) {
                writeEntry(out, entry);
            }
            for (Reference reference : page.children()) {
                writeReference(out, reference);
            }
            out.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(
                "aggregate inventory page could not be encoded",
                failure);
        }
    }

    private static byte[] encodeRoot(Root root) {
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            out.write(ROOT_MAGIC);
            out.writeByte(1);
            out.writeInt(root.totalCount());
            out.write(root.digest());
            out.write(root.declaredDigest());
            out.writeInt(root.topLevel());
            out.writeInt(root.topPages().size());
            for (Reference reference : root.topPages()) {
                writeReference(out, reference);
            }
            out.writeInt(root.pageCounts().length);
            for (int count : root.pageCounts()) {
                out.writeInt(count);
            }
            out.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(
                "aggregate inventory root could not be encoded",
                failure);
        }
    }

    private static void writeEntry(DataOutputStream out, Entry entry)
        throws IOException {
        out.writeInt(entry.index());
        writeString(out, entry.authorityKey());
        out.writeLong(entry.objectGeneration());
        out.writeLong(entry.sourceAuthorityOwnerGeneration());
        writeString(out, entry.expectedStoreVersion());
        out.writeInt(entry.ownerTransition().ordinal());
        out.write(entry.authorityPayloadSha256());
        out.write(entry.membershipMutationSha256());
    }

    private static Entry readEntry(DataInputStream in) throws IOException {
        int index = in.readInt();
        String authorityKey = readString(in);
        long objectGeneration = in.readLong();
        long sourceAuthorityOwnerGeneration = in.readLong();
        String expectedStoreVersion = readString(in);
        int transition = in.readInt();
        byte[] authorityPayloadSha256 = in.readNBytes(HASH_BYTES);
        byte[] membershipMutationSha256 = in.readNBytes(HASH_BYTES);
        if (authorityPayloadSha256.length != HASH_BYTES
            || membershipMutationSha256.length != HASH_BYTES
            || transition < 0
            || transition >= ZLinkAuthorityGenerationTransition.values().length) {
            throw new IOException("invalid aggregate inventory entry");
        }
        return new Entry(
            index,
            authorityKey,
            objectGeneration,
            sourceAuthorityOwnerGeneration,
            expectedStoreVersion,
            ZLinkAuthorityGenerationTransition.values()[transition],
            authorityPayloadSha256,
            membershipMutationSha256);
    }

    private static void writeReference(
        DataOutputStream out,
        Reference reference)
        throws IOException {
        out.writeInt(reference.level());
        out.writeInt(reference.index());
        out.writeInt(reference.startIndex());
        out.writeInt(reference.entryCount());
        out.write(reference.sha256());
    }

    private static Reference readReference(DataInputStream in)
        throws IOException {
        int level = in.readInt();
        int index = in.readInt();
        int startIndex = in.readInt();
        int entryCount = in.readInt();
        byte[] sha256 = in.readNBytes(HASH_BYTES);
        if (sha256.length != HASH_BYTES) {
            throw new IOException("truncated aggregate inventory reference");
        }
        return new Reference(level, index, startIndex, entryCount, sha256);
    }

    private static DataInputStream input(byte[] bytes, byte[] magic)
        throws IOException {
        if (bytes.length < magic.length + 1) {
            throw new IOException("aggregate inventory value is truncated");
        }
        var in = new DataInputStream(new ByteArrayInputStream(bytes));
        byte[] actualMagic = in.readNBytes(magic.length);
        if (!Arrays.equals(actualMagic, magic) || in.readUnsignedByte() != 1) {
            throw new IOException("aggregate inventory value header is invalid");
        }
        return in;
    }

    private static String readString(DataInputStream in) throws IOException {
        int length = in.readInt();
        if (length < 1 || length > 64 * 1024 || length > in.available()) {
            throw new IOException("aggregate inventory string length is invalid");
        }
        byte[] bytes = in.readNBytes(length);
        if (bytes.length != length) {
            throw new IOException("aggregate inventory string is truncated");
        }
        try {
            return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes))
                .toString();
        } catch (CharacterCodingException failure) {
            throw new IOException("aggregate inventory string is not UTF-8", failure);
        }
    }

    private static void writeString(DataOutputStream out, String value)
        throws IOException {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length < 1 || bytes.length > 64 * 1024) {
            throw new IOException("aggregate inventory string is out of bounds");
        }
        out.writeInt(bytes.length);
        out.write(bytes);
    }

    private static void writeBytes(DataOutputStream out, byte[] value)
        throws IOException {
        out.writeInt(value.length);
        out.write(value);
    }

    private static byte[] digestEntries(List<Entry> entries) {
        var digest = newDigest();
        for (Entry entry : entries) {
            digest.update(encodeEntry(entry));
        }
        return digest.digest();
    }

    private static byte[] encodeEntry(Entry entry) {
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            writeEntry(out, entry);
            out.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(
                "aggregate inventory entry could not be encoded",
                failure);
        }
    }

    static byte[] sha256(byte[] bytes) {
        return newDigest().digest(bytes);
    }

    private static MessageDigest newDigest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException failure) {
            throw new AssertionError(failure);
        }
    }

    private static ZLinkAggregateFence fence(
        ZLinkAggregatePrepareRequest request) {
        return new ZLinkAggregateFence(
            request.aggregateId(),
            request.aggregateGeneration());
    }

    private static ZLinkStoreKey pageKey(
        ZLinkAggregateFence fence,
        int level,
        int index) {
        return new ZLinkStoreKey(PREFIX + fence.aggregateId()
            + ":" + fence.aggregateGeneration() + ":page:"
            + level + ":" + index);
    }

    private static String inventoryPrefix(ZLinkAggregateFence fence) {
        return PREFIX + fence.aggregateId()
            + ":" + fence.aggregateGeneration() + ":";
    }

    private static ZLinkStoreKey participantPayloadKey(
        ZLinkAggregateFence fence,
        int index) {
        return new ZLinkStoreKey(PREFIX + fence.aggregateId()
            + ":" + fence.aggregateGeneration() + ":participant:"
            + index + ":payload");
    }

    private static ZLinkStoreKey participantMembershipKey(
        ZLinkAggregateFence fence,
        int index) {
        return new ZLinkStoreKey(PREFIX + fence.aggregateId()
            + ":" + fence.aggregateGeneration() + ":participant:"
            + index + ":membership");
    }

    private static IllegalStateException dataLost(
        ZLinkAggregateFence fence,
        String message) {
        return new IllegalStateException(
            "aggregate inventory data lost for " + fence + ": " + message);
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private record Tree(Root root, List<Page> pages) {
    }

    private record Root(
        int totalCount,
        byte[] digest,
        byte[] declaredDigest,
        int topLevel,
        List<Reference> topPages,
        int[] pageCounts) {
        Root {
            digest = digest.clone();
            declaredDigest = declaredDigest.clone();
            topPages = List.copyOf(topPages);
            pageCounts = pageCounts.clone();
        }

        @Override public byte[] digest() { return digest.clone(); }
        @Override public byte[] declaredDigest() { return declaredDigest.clone(); }
        @Override public int[] pageCounts() { return pageCounts.clone(); }
    }

    private record Page(
        int level,
        int index,
        int startIndex,
        int entryCount,
        List<Entry> entries,
        List<Reference> children) {
        Page {
            entries = List.copyOf(entries);
            children = List.copyOf(children);
        }
    }

    private record Reference(
        int level,
        int index,
        int startIndex,
        int entryCount,
        byte[] sha256) {
        Reference {
            sha256 = sha256.clone();
        }

        @Override public byte[] sha256() { return sha256.clone(); }
    }

    private record Entry(
        int index,
        String authorityKey,
        long objectGeneration,
        long sourceAuthorityOwnerGeneration,
        String expectedStoreVersion,
        ZLinkAuthorityGenerationTransition ownerTransition,
        byte[] authorityPayloadSha256,
        byte[] membershipMutationSha256) {
        Entry {
            authorityPayloadSha256 = authorityPayloadSha256.clone();
            membershipMutationSha256 = membershipMutationSha256.clone();
        }

        @Override public byte[] authorityPayloadSha256() {
            return authorityPayloadSha256.clone();
        }

        @Override public byte[] membershipMutationSha256() {
            return membershipMutationSha256.clone();
        }
    }
}
