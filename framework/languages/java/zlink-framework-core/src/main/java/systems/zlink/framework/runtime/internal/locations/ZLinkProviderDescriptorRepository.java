package systems.zlink.framework.runtime.internal.locations;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.BiPredicate;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.locations.ZLinkPageRequest;

/**
 * Translates Framework descriptor records to opaque provider operations.
 *
 * <p>Descriptor generation and owner fencing remain private Framework data.
 * The provider only sees versioned bytes and atomic conditions.</p>
 */
final class ZLinkProviderDescriptorRepository {
    private static final String PREFIX = "zlink:v11:";
    private static final int OWNER_LEASE_WRITE_RETRIES = 3;
    private static final int DEFAULT_MESH_PAGE = 100;
    private static final int DEFAULT_CHANNEL_PAGE = 256;
    private static final int MAXIMUM_PAGE_SIZE = 1000;
    private static final int MAXIMUM_CONTINUATION_CHARACTERS = 5600;
    private final ZLinkLocationStore provider;

    ZLinkProviderDescriptorRepository(ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        return update(
            meshKey(descriptor.meshName(), descriptor.rid()),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor,
            intent,
            value -> encode(
                value.generation(),
                ZLinkLocationDescriptorCodec.serializeMeshNode(
                    value.descriptor())),
            bytes -> {
                Envelope envelope = decodeEnvelope(bytes);
                return new StoredDescriptor<>(
                    envelope.generation(),
                    ZLinkLocationDescriptorCodec.deserializeMeshNode(
                        envelope.json(),
                        extractLifecycle(envelope.json()),
                        extractUpdatedAt(envelope.json())));
            },
            ZLinkProviderDescriptorRepository::sameImmutableMesh);
    }

    CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return remove(
            meshKey(key.meshName(), key.rid()),
            owner,
            bytes -> decodeMesh(bytes).descriptor().ownerId(),
            bytes -> decodeMesh(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
        String meshName,
        ZLinkPageRequest page) {
        return list(
            meshPrefix(meshName),
            page,
            DEFAULT_MESH_PAGE,
            bytes -> decodeMesh(bytes).descriptor());
    }

    CompletionStage<ZLinkLocationWriteResult> updateClientServer(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        return update(
            clientServerKey(
                descriptor.channelName(),
                descriptor.serverRid()),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor,
            intent,
            value -> encode(
                value.generation(),
                ZLinkLocationDescriptorCodec.serializeClientServer(
                    value.descriptor())),
            bytes -> {
                Envelope envelope = decodeEnvelope(bytes);
                return new StoredDescriptor<>(
                    envelope.generation(),
                    ZLinkLocationDescriptorCodec.deserializeClientServer(
                        envelope.json(),
                        0,
                        extractUpdatedAt(envelope.json())));
            },
            (current, next) ->
                current.endpoint().equals(next.endpoint())
                    && current.securityIdentity().equals(
                        next.securityIdentity()));
    }

    CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return remove(
            clientServerKey(key.channelName(), key.serverRid()),
            owner,
            bytes -> decodeClientServer(bytes).descriptor().ownerId(),
            bytes -> decodeClientServer(bytes)
                .descriptor().leaseGeneration());
    }

    CompletionStage<
        ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        listClientServers(String channelName, ZLinkPageRequest page) {
        return list(
            clientServerPrefix(channelName),
            page,
            DEFAULT_CHANNEL_PAGE,
            bytes -> decodeClientServer(bytes).descriptor());
    }

    CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        return update(
            fanoutKey(descriptor.channelName(), descriptor.publisherRid()),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor,
            intent,
            value -> encode(
                value.generation(),
                ZLinkLocationDescriptorCodec.serializeFanoutPublisher(
                    value.descriptor())),
            bytes -> {
                Envelope envelope = decodeEnvelope(bytes);
                return new StoredDescriptor<>(
                    envelope.generation(),
                    ZLinkLocationDescriptorCodec.deserializeFanoutPublisher(
                        envelope.json(),
                        0,
                        extractUpdatedAt(envelope.json())));
            },
            (current, next) ->
                current.endpoint().equals(next.endpoint())
                    && current.securityIdentity().equals(
                        next.securityIdentity()));
    }

    CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return remove(
            fanoutKey(key.channelName(), key.publisherRid()),
            owner,
            bytes -> decodeFanout(bytes).descriptor().ownerId(),
            bytes -> decodeFanout(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        listFanoutPublishers(String channelName, ZLinkPageRequest page) {
        return list(
            fanoutPrefix(channelName),
            page,
            DEFAULT_CHANNEL_PAGE,
            bytes -> decodeFanout(bytes).descriptor());
    }

    private <T> CompletionStage<ZLinkLocationWriteResult> update(
        ZLinkStoreKey rowKey,
        String ownerId,
        long leaseGeneration,
        long lifecycleGeneration,
        long descriptorRevision,
        T descriptor,
        ZLinkLocationWriteIntent intent,
        Function<StoredDescriptor<T>, byte[]> encode,
        Function<byte[], StoredDescriptor<T>> decode,
        BiPredicate<T, T> immutableFieldsEqual) {
        Objects.requireNonNull(intent, "intent");
        ZLinkStoreKey leaseKey = ownerKey(ownerId);
        return provider.read(leaseKey, active()).thenCompose(lease -> {
            if (!(lease instanceof ZLinkStoreReadFound liveLease)
                || decodeOwnerGeneration(liveLease.value().bytes())
                    != leaseGeneration) {
                return completed(
                    ZLinkLocationWriteResult.ignoredStale());
            }
            return provider.read(rowKey, active()).thenCompose(current -> {
                long generation = 1;
                ZLinkStoreCondition rowCondition;
                if (current instanceof ZLinkStoreReadFound found) {
                    StoredDescriptor<T> record =
                        decode.apply(found.value().bytes());
                    generation = record.generation();
                    T stored = record.descriptor();
                    DescriptorIdentity identity =
                        identity(stored);
                    boolean renew =
                        intent == ZLinkLocationWriteIntent.RENEW
                            && identity.ownerId().equals(ownerId)
                            && identity.leaseGeneration()
                                == leaseGeneration
                            && identity.lifecycleGeneration()
                                == lifecycleGeneration
                            && descriptorRevision
                                > identity.descriptorRevision()
                            && immutableFieldsEqual.test(
                                stored,
                                descriptor);
                    if (!renew) {
                        if (intent
                            == ZLinkLocationWriteIntent.NEW_CLAIM) {
                            return completed(
                                ZLinkLocationWriteResult
                                    .rejectedConflict());
                        }
                        return provider.read(
                                ownerKey(identity.ownerId()),
                                active())
                            .thenCompose(previousOwner -> {
                                if (previousOwner
                                    instanceof ZLinkStoreReadFound) {
                                    return completed(
                                        ZLinkLocationWriteResult
                                            .ignoredStale());
                                }
                                return writeDescriptor(
                                    rowKey,
                                    liveLease,
                                    new ZLinkStoreVersionCondition(
                                        rowKey,
                                        found.value().version()),
                                    encode.apply(
                                        new StoredDescriptor<>(
                                            Math.addExact(
                                                record.generation(),
                                                1),
                                            descriptor)),
                                    Math.addExact(
                                        record.generation(),
                                        1),
                                    OWNER_LEASE_WRITE_RETRIES);
                            });
                    }
                    rowCondition = new ZLinkStoreVersionCondition(
                        rowKey,
                        found.value().version());
                } else {
                    if (intent == ZLinkLocationWriteIntent.RENEW) {
                        return completed(
                            ZLinkLocationWriteResult.ignoredStale());
                    }
                    rowCondition =
                        new ZLinkStoreMissingCondition(rowKey);
                }
                byte[] encoded = encode.apply(
                    new StoredDescriptor<>(generation, descriptor));
                return writeDescriptor(
                    rowKey,
                    liveLease,
                    rowCondition,
                    encoded,
                    generation,
                    OWNER_LEASE_WRITE_RETRIES);
            });
        });
    }

    private CompletionStage<ZLinkLocationWriteResult> writeDescriptor(
        ZLinkStoreKey rowKey,
        ZLinkStoreReadFound liveLease,
        ZLinkStoreCondition rowCondition,
        byte[] encoded,
        long generation,
        int retriesRemaining) {
        var request = new ZLinkStoreWriteRequest(
            List.of(
                new ZLinkStoreVersionCondition(
                    ownerKey(decodeOwnerId(
                        liveLease.value().bytes())),
                    liveLease.value().version()),
                rowCondition),
            List.of(new ZLinkStorePut(rowKey, encoded, null)));
        CompletionStage<systems.zlink.framework.locationprovider
            .ZLinkStoreWriteResult> write;
        try {
            write = provider.write(request, active());
        } catch (Throwable failure) {
            return reconcile(rowKey, encoded, generation, failure);
        }
        return write.handle((result, failure) -> {
            if (failure != null) {
                return reconcile(
                    rowKey,
                    encoded,
                    generation,
                    unwrap(failure));
            }
            if (result instanceof ZLinkStoreWriteApplied applied) {
                return completed(ZLinkLocationWriteResult.stored(
                    generation,
                    applied.storeNow()));
            }
            if (retriesRemaining <= 0) {
                return completed(ZLinkLocationWriteResult.ignoredStale());
            }
            String ownerId = decodeOwnerId(liveLease.value().bytes());
            long leaseGeneration =
                decodeOwnerGeneration(liveLease.value().bytes());
            return provider.read(ownerKey(ownerId), active())
                .thenCompose(currentLease -> {
                    if (!(currentLease
                            instanceof ZLinkStoreReadFound refreshed)
                        || decodeOwnerGeneration(
                                refreshed.value().bytes())
                            != leaseGeneration) {
                        return completed(
                            ZLinkLocationWriteResult.ignoredStale());
                    }
                    return writeDescriptor(
                        rowKey,
                        refreshed,
                        rowCondition,
                        encoded,
                        generation,
                        retriesRemaining - 1);
                });
        }).thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkLocationWriteResult> reconcile(
        ZLinkStoreKey rowKey,
        byte[] expected,
        long generation,
        Throwable originalFailure) {
        return provider.read(rowKey, active())
            .toCompletableFuture()
            .orTimeout(5, TimeUnit.SECONDS)
            .handle((result, failure) -> {
                if (failure == null
                    && result instanceof ZLinkStoreReadFound found
                    && java.util.Arrays.equals(
                        expected,
                        found.value().bytes())) {
                    return ZLinkLocationWriteResult.stored(
                        generation,
                        found.value().storeNow());
                }
                throw new CompletionException(originalFailure);
            });
    }

    private CompletionStage<ZLinkLocationWriteStatus> remove(
        ZLinkStoreKey rowKey,
        ZLinkLocationOwnerToken owner,
        Function<byte[], String> ownerId,
        Function<byte[], Long> leaseGeneration) {
        Objects.requireNonNull(owner, "owner");
        return provider.read(rowKey, active()).thenCompose(current -> {
            if (!(current instanceof ZLinkStoreReadFound found)
                || !owner.ownerId().equals(
                    ownerId.apply(found.value().bytes()))
                || owner.leaseGeneration()
                    != leaseGeneration.apply(found.value().bytes())) {
                return completed(
                    ZLinkLocationWriteStatus.IGNORED_STALE);
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(
                            rowKey,
                            found.value().version())),
                        List.of(new ZLinkStoreDelete(rowKey))),
                    active())
                .thenApply(result -> result
                    instanceof ZLinkStoreWriteApplied
                    ? ZLinkLocationWriteStatus.STORED
                    : ZLinkLocationWriteStatus.IGNORED_STALE);
        });
    }

    private <T> CompletionStage<ZLinkLocationPage<T>> list(
        String prefix,
        ZLinkPageRequest request,
        int defaultPageSize,
        Function<byte[], T> decode) {
        Objects.requireNonNull(request, "request");
        int pageSize = request.pageSize() <= 0
            ? defaultPageSize
            : request.pageSize();
        if (pageSize < 1 || pageSize > MAXIMUM_PAGE_SIZE) {
            throw new IllegalArgumentException(
                "pageSize must be in the range 1..1000");
        }
        ZLinkStoreScanCursor cursor =
            request.continuationToken() == null
                ? null
                : decodeContinuation(
                    prefix,
                    request.continuationToken());
        return provider.scan(
                new ZLinkStoreScanRequest(
                    prefix,
                    cursor,
                    pageSize),
                active())
            .thenApply(result -> {
                if (result instanceof ZLinkStoreScanExpired) {
                    throw new CompletionException(new IOException(
                        "Location Store snapshot has expired."));
                }
                var page = ((ZLinkStoreScanPageResult) result).value();
                if (page.items().size() > pageSize) {
                    throw new IllegalStateException(
                        "Location Store returned more rows than requested.");
                }
                List<T> items = page.items().stream()
                    .map(item -> decode.apply(
                        item.value().bytes()))
                    .toList();
                return new ZLinkLocationPage<>(
                    items,
                    page.nextCursor() == null
                        ? null
                        : encodeContinuation(
                            prefix,
                            page.nextCursor()));
            });
    }

    private static StoredDescriptor<ZLinkMeshNodeDescriptor> decodeMesh(
        byte[] bytes) {
        Envelope envelope = decodeEnvelope(bytes);
        return new StoredDescriptor<>(
            envelope.generation(),
            ZLinkLocationDescriptorCodec.deserializeMeshNode(
                envelope.json(),
                extractLifecycle(envelope.json()),
                extractUpdatedAt(envelope.json())));
    }

    private static StoredDescriptor<ZLinkClientServerServerDescriptor>
        decodeClientServer(byte[] bytes) {
        Envelope envelope = decodeEnvelope(bytes);
        return new StoredDescriptor<>(
            envelope.generation(),
            ZLinkLocationDescriptorCodec.deserializeClientServer(
                envelope.json(),
                0,
                extractUpdatedAt(envelope.json())));
    }

    private static StoredDescriptor<ZLinkFanoutPublisherDescriptor>
        decodeFanout(byte[] bytes) {
        Envelope envelope = decodeEnvelope(bytes);
        return new StoredDescriptor<>(
            envelope.generation(),
            ZLinkLocationDescriptorCodec.deserializeFanoutPublisher(
                envelope.json(),
                0,
                extractUpdatedAt(envelope.json())));
    }

    private static DescriptorIdentity identity(Object descriptor) {
        if (descriptor instanceof ZLinkMeshNodeDescriptor value) {
            return new DescriptorIdentity(
                value.ownerId(),
                value.leaseGeneration(),
                value.lifecycleGeneration(),
                value.descriptorRevision());
        }
        if (descriptor
            instanceof ZLinkClientServerServerDescriptor value) {
            return new DescriptorIdentity(
                value.ownerId(),
                value.leaseGeneration(),
                value.lifecycleGeneration(),
                value.descriptorRevision());
        }
        ZLinkFanoutPublisherDescriptor value =
            (ZLinkFanoutPublisherDescriptor) descriptor;
        return new DescriptorIdentity(
            value.ownerId(),
            value.leaseGeneration(),
            value.lifecycleGeneration(),
            value.descriptorRevision());
    }

    private static boolean sameImmutableMesh(
        ZLinkMeshNodeDescriptor current,
        ZLinkMeshNodeDescriptor next) {
        return current.meshName().equals(next.meshName())
            && current.rid().equals(next.rid())
            && current.lifecycleGeneration()
                == next.lifecycleGeneration()
            && current.endpoint().equals(next.endpoint())
            && current.channelWeights().keySet().equals(
                next.channelWeights().keySet())
            && current.applicationVersion()
                == next.applicationVersion()
            && current.objectCapabilities().equals(
                next.objectCapabilities())
            && current.objectRole() == next.objectRole()
            && current.entrySpotId().equals(next.entrySpotId())
            && current.capacity().actors().limit()
                == next.capacity().actors().limit()
            && current.capacity().spots().limit()
                == next.capacity().spots().limit()
            && current.securityIdentity().equals(
                next.securityIdentity())
            && current.ownerId().equals(next.ownerId())
            && current.leaseGeneration()
                == next.leaseGeneration();
    }

    private static byte[] encode(long generation, String json) {
        byte[] payload = json.getBytes(StandardCharsets.UTF_8);
        return ByteBuffer.allocate(Long.BYTES + payload.length)
            .putLong(generation)
            .put(payload)
            .array();
    }

    private static Envelope decodeEnvelope(byte[] bytes) {
        if (bytes.length <= Long.BYTES) {
            throw new IllegalStateException(
                "Location descriptor record is invalid");
        }
        ByteBuffer input = ByteBuffer.wrap(bytes);
        long generation = input.getLong();
        byte[] payload = new byte[input.remaining()];
        input.get(payload);
        if (generation <= 0) {
            throw new IllegalStateException(
                "Location descriptor generation is invalid");
        }
        return new Envelope(
            generation,
            new String(payload, StandardCharsets.UTF_8));
    }

    private static long extractLifecycle(String json) {
        try {
            return new com.fasterxml.jackson.databind.ObjectMapper()
                .readTree(json)
                .path("LifecycleGeneration")
                .asLong();
        } catch (com.fasterxml.jackson.core.JsonProcessingException error) {
            throw new IllegalStateException(
                "Location descriptor record is invalid",
                error);
        }
    }

    private static Instant extractUpdatedAt(String json) {
        try {
            return Instant.parse(
                new com.fasterxml.jackson.databind.ObjectMapper()
                    .readTree(json)
                    .path("UpdatedAt")
                    .asText());
        } catch (RuntimeException
            | com.fasterxml.jackson.core.JsonProcessingException error) {
            throw new IllegalStateException(
                "Location descriptor record is invalid",
                error);
        }
    }

    private static long decodeOwnerGeneration(byte[] bytes) {
        ByteBuffer input = ownerRecord(bytes);
        int length = input.getInt();
        input.position(input.position() + length);
        return input.getLong();
    }

    private static String decodeOwnerId(byte[] bytes) {
        ByteBuffer input = ownerRecord(bytes);
        int length = input.getInt();
        byte[] owner = new byte[length];
        input.get(owner);
        return new String(owner, StandardCharsets.UTF_8);
    }

    private static ByteBuffer ownerRecord(byte[] bytes) {
        try {
            ByteBuffer input = ByteBuffer.wrap(bytes);
            int length = input.getInt(0);
            if (length < 1
                || length != bytes.length - Integer.BYTES - Long.BYTES) {
                throw new IllegalStateException();
            }
            long generation = input.getLong(
                Integer.BYTES + length);
            if (generation <= 0) {
                throw new IllegalStateException();
            }
            return input;
        } catch (RuntimeException error) {
            throw new IllegalStateException(
                "Location Store owner lease record is invalid",
                error);
        }
    }

    private static ZLinkStoreKey ownerKey(String ownerId) {
        return new ZLinkStoreKey(
            PREFIX + "owner:" + segment(ownerId));
    }

    private static ZLinkStoreKey meshKey(
        String meshName,
        RoutingId rid) {
        return new ZLinkStoreKey(
            meshPrefix(meshName) + segment(rid.toHex()));
    }

    private static String meshPrefix(String meshName) {
        return PREFIX + "mesh:" + segment(meshName);
    }

    private static ZLinkStoreKey clientServerKey(
        String channelName,
        RoutingId rid) {
        return new ZLinkStoreKey(
            clientServerPrefix(channelName)
                + segment(rid.toHex()));
    }

    private static String clientServerPrefix(String channelName) {
        return PREFIX + "client-server:" + segment(channelName);
    }

    private static ZLinkStoreKey fanoutKey(
        String channelName,
        RoutingId rid) {
        return new ZLinkStoreKey(
            fanoutPrefix(channelName) + segment(rid.toHex()));
    }

    private static String fanoutPrefix(String channelName) {
        return PREFIX + "fanout:" + segment(channelName);
    }

    private static String segment(String value) {
        return value.getBytes(StandardCharsets.UTF_8).length
            + ":" + value + ":";
    }

    private static String encodeContinuation(
        String prefix,
        ZLinkStoreScanCursor cursor) {
        byte[] cursorBytes =
            cursor.value().getBytes(StandardCharsets.UTF_8);
        if (cursorBytes.length < 1
            || cursorBytes.length > 4096) {
            throw new IllegalStateException(
                "Location Store returned an invalid snapshot cursor.");
        }
        return "v1." + base64(digest(prefix)) + "."
            + base64(cursorBytes);
    }

    private static ZLinkStoreScanCursor decodeContinuation(
        String prefix,
        String token) {
        if (token.length() < 1
            || token.length() > MAXIMUM_CONTINUATION_CHARACTERS) {
            throw invalidContinuation();
        }
        String[] parts = token.split("\\.", 3);
        if (parts.length != 3
            || !"v1".equals(parts[0])) {
            throw invalidContinuation();
        }
        try {
            byte[] expected = digest(prefix);
            byte[] actual = decodeBase64(parts[1]);
            byte[] cursor = decodeBase64(parts[2]);
            if (!MessageDigest.isEqual(expected, actual)
                || cursor.length < 1
                || cursor.length > 4096) {
                throw invalidContinuation();
            }
            String value = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(cursor))
                .toString();
            return new ZLinkStoreScanCursor(value);
        } catch (IllegalArgumentException
            | CharacterCodingException error) {
            throw invalidContinuation();
        }
    }

    private static byte[] digest(String prefix) {
        try {
            return MessageDigest.getInstance("SHA-256")
                .digest(prefix.getBytes(StandardCharsets.UTF_8));
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException(
                "SHA-256 is required",
                error);
        }
    }

    private static String base64(byte[] bytes) {
        return Base64.getUrlEncoder().withoutPadding()
            .encodeToString(bytes);
    }

    private static byte[] decodeBase64(String value) {
        return Base64.getUrlDecoder().decode(value);
    }

    private static IllegalArgumentException invalidContinuation() {
        return new IllegalArgumentException(
            "Location Store continuation token is invalid.");
    }

    private static systems.zlink.framework.locationprovider
        .ZLinkStoreCancellation active() {
        return () -> false;
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException completion
            && completion.getCause() != null
            ? completion.getCause()
            : failure;
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private record Envelope(long generation, String json) {}

    private record StoredDescriptor<T>(
        long generation,
        T descriptor) {}

    private record DescriptorIdentity(
        String ownerId,
        long leaseGeneration,
        long lifecycleGeneration,
        long descriptorRevision) {}
}
