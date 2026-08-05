package systems.zlink.framework.locations.redis;

import systems.zlink.framework.runtime.internal.locations.ZLinkLocationDescriptorCodec;

import com.fasterxml.jackson.databind.ObjectMapper;
import io.lettuce.core.RedisCommandExecutionException;
import io.lettuce.core.RedisCommandTimeoutException;
import io.lettuce.core.RedisConnectionException;
import io.lettuce.core.RedisException;
import io.lettuce.core.ScriptOutputType;
import io.lettuce.core.KeyValue;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLease;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewal;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseSnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;

final class ZLinkRedisLocationScriptsClient {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ZLinkRedisLocationConnection connection;
    private final ZLinkRedisLocationKeys keys;

    ZLinkRedisLocationScriptsClient(
        ZLinkRedisLocationConnection connection,
        ZLinkRedisLocationKeys keys) {
        this.connection = connection;
        this.keys = keys;
    }

    CompletionStage<OptionalLong> getMeshNodeChangeStamp(
        String meshName) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName must be non-blank");
        }
        return connection.commands()
            .thenCompose(redis -> redis.get(
                keys.stampKey("mesh-node", meshName)))
            .thenApply(value -> value == null
                ? OptionalLong.empty()
                : OptionalLong.of(Long.parseLong(value)));
    }

    CompletionStage<ZLinkOwnerLeaseRenewal> renewOwnerLeaseAsync(
        String ownerId,
        RoutingId nodeRid,
        Duration leaseTtl) {
        long ttlMs = Math.max(1L, leaseTtl.toMillis());
        return connection.commands()
            .thenCompose(redis -> redis.<Long>eval(
                ZLinkRedisLocationScripts.RENEW_LEASE,
                ScriptOutputType.INTEGER,
                new String[] {
                    keys.legacyLeaseKey(ownerId),
                    keys.leaseIndexKey()
                },
                ownerId,
                nodeRid.toHex(),
                Long.toString(ttlMs)))
            .thenApply(nowMs -> {
                Instant storeNow = fromUnixMs(nowMs);
                return new ZLinkOwnerLeaseRenewal(storeNow.plusMillis(ttlMs), storeNow);
            })
            .exceptionally(ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLeaseAsync(
        String ownerId,
        Duration leaseTtl) {
        long ttlMs = Math.max(1L, leaseTtl.toMillis());
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.CLAIM_OWNER_LEASE,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.leaseKey(ownerId),
                    keys.leaseGenerationKey()
                },
                ownerId,
                Long.toString(ttlMs)))
            .<ZLinkOwnerLeaseClaimResult>thenApply(raw -> {
                String status = string(raw.getFirst());
                return switch (status) {
                    case "claimed" -> new ZLinkOwnerLeaseClaimed(
                        new ZLinkLocationOwnerToken(
                            ownerId,
                            number(raw.get(1))),
                        fromUnixMs(number(raw.get(2))),
                        fromUnixMs(number(raw.get(3))));
                    case "generation-exhausted" ->
                        new ZLinkOwnerLeaseGenerationExhausted();
                    default -> new ZLinkOwnerLeaseClaimConflict();
                };
            })
            .exceptionally(
                ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLeaseAsync(
        String ownerId) {
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.READ_OWNER_LEASE,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.leaseKey(ownerId)
                },
                ownerId))
            .<ZLinkOwnerLeaseReadResult>thenApply(raw -> "found".equals(string(raw.getFirst()))
                ? new ZLinkOwnerLeaseFound(
                    new ZLinkLocationOwnerToken(
                        ownerId,
                        number(raw.get(1))),
                    fromUnixMs(number(raw.get(2))),
                    fromUnixMs(number(raw.get(3))))
                : new ZLinkOwnerLeaseMissing())
            .exceptionally(
                ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        Duration leaseTtl) {
        long ttlMs = Math.max(1L, leaseTtl.toMillis());
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.RENEW_OWNER_LEASE,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.leaseKey(token.ownerId())
                },
                token.ownerId(),
                Long.toString(token.leaseGeneration()),
                Long.toString(ttlMs)))
            .<ZLinkOwnerLeaseRenewResult>thenApply(raw -> "renewed".equals(string(raw.getFirst()))
                ? new ZLinkOwnerLeaseRenewed(
                    fromUnixMs(number(raw.get(1))),
                    fromUnixMs(number(raw.get(2))))
                : new ZLinkOwnerLeaseRenewStale())
            .exceptionally(
                ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLeaseAsync(
        ZLinkLocationOwnerToken token) {
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.RELEASE_OWNER_LEASE,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.leaseKey(token.ownerId())
                },
                token.ownerId(),
                Long.toString(token.leaseGeneration())))
            .thenApply(raw -> "released".equals(string(raw.getFirst()))
                ? ZLinkOwnerLeaseReleaseResult.RELEASED
                : ZLinkOwnerLeaseReleaseResult.STALE)
            .exceptionally(
                ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<Boolean> removeOwnerLeaseAsync(String ownerId) {
        return connection.commands()
            .thenCompose(redis -> redis.<Long>eval(
                ZLinkRedisLocationScripts.REMOVE_LEASE,
                ScriptOutputType.INTEGER,
                new String[] {
                    keys.legacyLeaseKey(ownerId),
                    keys.leaseIndexKey()
                },
                ownerId))
            .thenApply(removed -> removed != null && removed > 0)
            .exceptionally(ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteResult> writeMeshNode(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        ZLinkMeshNodeDescriptorKey key =
            new ZLinkMeshNodeDescriptorKey(
                descriptor.meshName(),
                descriptor.rid());
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(key);
        String json =
            ZLinkLocationDescriptorCodec.serializeMeshNode(descriptor);
        if (json.getBytes(java.nio.charset.StandardCharsets.UTF_8)
            .length > 1024 * 1024) {
            throw new IllegalArgumentException(
                "encoded MeshNode descriptor exceeds 1 MiB");
        }
        String descriptorRow =
            keys.rowHashKey("mesh-node", rowKey);
        String entryAuthorityKey = descriptor.entrySpotId()
            .map(systems.zlink.framework.runtime.locations
                .ZLinkAuthorityKeyCodec::spot)
            .orElse("");
        String entryClaimKey = entryAuthorityKey.isEmpty()
            ? keys.schemaKey()
            : keys.entrySpotIdentityClaimKey(
                descriptor.entrySpotId().orElseThrow());
        return connection.commands()
            .thenCompose(redis -> redis.hget(
                    descriptorRow,
                    "owner")
                .thenCompose(previousOwner -> {
                    CompletionStage<String> previousLease =
                        previousOwner == null
                            ? CompletableFuture.completedFuture(null)
                            : redis.hget(
                                keys.meshNodeDescriptorMetadataKey(
                                    key),
                                "ownerLeaseGeneration");
                    return previousLease.thenCompose(
                        previousLeaseGeneration -> redis.hget(
                            keys.meshNodeDescriptorMetadataKey(key),
                            "entrySpotId").thenCompose(
                                previousEntrySpotId -> redis.hget(
                            entryClaimKey,
                            "ownerId").thenCompose(claimOwner ->
                            redis.<List<Object>>eval(
                    ZLinkRedisLocationScripts.WRITE_MESH_NODE,
                    ScriptOutputType.MULTI,
                    new String[] {
                        descriptorRow,
                        keys.kindIndexKey("mesh-node"),
                        keys.leaseKey(descriptor.ownerId()),
                        keys.meshNodeDescriptorMetadataKey(key),
                        keys.meshNodeOwnerTokenIndexKey(
                            descriptor.ownerId(),
                            descriptor.leaseGeneration()),
                        previousOwner == null
                            || previousLeaseGeneration == null
                            ? keys.schemaKey()
                            : keys.meshNodeOwnerTokenIndexKey(
                                previousOwner,
                                Long.parseLong(
                                    previousLeaseGeneration)),
                        keys.stampKey(
                            "mesh-node",
                            descriptor.meshName()),
                        keys.stampKey("mesh-node", null),
                        previousOwner == null
                            ? keys.schemaKey()
                            : keys.leaseKey(previousOwner),
                        entryClaimKey,
                        entryAuthorityKey.isEmpty()
                            ? keys.schemaKey()
                            : keys.authorityRowKey(entryAuthorityKey),
                        claimOwner == null
                            ? keys.schemaKey()
                            : keys.leaseKey(claimOwner),
                        previousEntrySpotId == null
                            || previousEntrySpotId.isEmpty()
                            ? keys.meshNodeDescriptorMetadataKey(key)
                            : keys.entrySpotIdentityClaimKey(
                                previousEntrySpotId)
                    },
                intentName(intent),
                descriptor.ownerId(),
                Long.toString(descriptor.leaseGeneration()),
                Long.toString(descriptor.lifecycleGeneration()),
                Long.toString(descriptor.descriptorRevision()),
                ZLinkLocationDescriptorCodec
                    .meshNodeImmutableFingerprint(descriptor),
                json,
                rowKey,
                descriptor.meshName(),
                descriptor.objectRole().name()
                    .toLowerCase(java.util.Locale.ROOT),
                Integer.toString(descriptor.state().wireValue()),
                Long.toString(descriptor.applicationVersion()),
                rowKey,
                ZLinkLocationDescriptorCodec
                    .serializeMeshNodeAdmissionCapabilities(
                        descriptor.objectCapabilities()),
                Integer.toString(
                    descriptor.capacity().actors().limit()),
                Integer.toString(
                    descriptor.capacity().spots().limit()),
                Integer.toString(
                    descriptor.activationConcurrency().limit()),
                descriptor.entrySpotId().orElse(""),
                entryAuthorityKey))));
                }))
            .thenApply(ZLinkRedisLocationScriptsClient::toWriteResult)
            .exceptionally(
                ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(key);
        return connection.commands()
            .thenCompose(redis -> redis.hget(
                    keys.meshNodeDescriptorMetadataKey(key),
                    "entrySpotId")
                .thenCompose(entrySpotId -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.REMOVE_MESH_NODE,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.rowHashKey("mesh-node", rowKey),
                    keys.kindIndexKey("mesh-node"),
                    keys.meshNodeDescriptorMetadataKey(key),
                    keys.meshNodeOwnerTokenIndexKey(
                        owner.ownerId(),
                        owner.leaseGeneration()),
                    keys.stampKey("mesh-node", key.meshName()),
                    keys.stampKey("mesh-node", null),
                    entrySpotId == null || entrySpotId.isEmpty()
                        ? keys.schemaKey()
                        : keys.entrySpotIdentityClaimKey(entrySpotId),
                    keys.schemaKey()
                },
                owner.ownerId(),
                Long.toString(owner.leaseGeneration()),
                rowKey,
                keys.ownerIndexKeyPrefix("mesh-node"),
                keys.stampKey("mesh-node", key.meshName()),
                keys.stampKey("mesh-node", null),
                keys.meshNodeDescriptorStorageId(rowKey))))
            .thenApply(raw ->
                "stored".equals(string(raw.getFirst()))
                    ? ZLinkLocationWriteStatus.STORED
                    : ZLinkLocationWriteStatus.IGNORED_STALE)
            .exceptionally(
                ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteResult> writeClientServer(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        ZLinkClientServerServerDescriptorKey key =
            new ZLinkClientServerServerDescriptorKey(
                descriptor.channelName(),
                descriptor.serverRid());
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeClientServerKey(key);
        String json =
            ZLinkLocationDescriptorCodec.serializeClientServer(
                descriptor);
        if (json.getBytes(StandardCharsets.UTF_8).length
            > 1024 * 1024) {
            throw new IllegalArgumentException(
                "encoded ClientServer descriptor exceeds 1 MiB");
        }
        String descriptorRow =
            keys.clientServerDescriptorRowKey(rowKey);
        String metadata =
            keys.clientServerDescriptorMetadataKey(rowKey);
        return connection.commands()
            .thenCompose(redis -> redis.hmget(
                    metadata,
                    "ownerId",
                    "ownerLeaseGeneration")
                .thenCompose(previous -> {
                    String previousOwner =
                        hashField(previous, "ownerId");
                    String previousLease =
                        hashField(
                            previous,
                            "ownerLeaseGeneration");
                    String placeholder = keys.schemaKey();
                    return redis.<List<Object>>eval(
                        ZLinkRedisLocationScripts
                            .WRITE_CLIENT_SERVER,
                        ScriptOutputType.MULTI,
                        new String[] {
                            descriptorRow,
                            metadata,
                            keys.clientServerDescriptorIndexKey(),
                            keys.leaseKey(descriptor.ownerId()),
                            keys.counterKey(),
                            keys.clientServerOwnerTokenIndexKey(
                                descriptor.ownerId(),
                                descriptor.leaseGeneration()),
                            previousOwner == null
                                ? placeholder
                                : keys.leaseKey(previousOwner),
                            previousOwner == null
                                || previousLease == null
                                ? placeholder
                                : keys
                                    .clientServerOwnerTokenIndexKey(
                                        previousOwner,
                                        Long.parseLong(
                                            previousLease)),
                            keys.clientServerDescriptorGlobalStampKey(),
                            keys.clientServerDescriptorStampKey(
                                descriptor.channelName()),
                            keys.clientServerDescriptorChannelIndexKey(
                                descriptor.channelName())
                        },
                        intentName(intent),
                        descriptor.ownerId(),
                        Long.toString(
                            descriptor.leaseGeneration()),
                        Long.toString(
                            descriptor.lifecycleGeneration()),
                        Long.toString(
                            descriptor.descriptorRevision()),
                        ZLinkLocationDescriptorCodec
                            .clientServerImmutableFingerprint(
                                descriptor),
                        json,
                        descriptor.state().name()
                            .toLowerCase(
                                java.util.Locale.ROOT),
                        Integer.toString(descriptor.weight()),
                        descriptor.channelName(),
                        rowKey,
                        previousOwner == null
                            ? ""
                            : previousOwner,
                        previousLease == null
                            ? ""
                            : previousLease);
                }))
            .thenApply(
                ZLinkRedisLocationScriptsClient::toWriteResult)
            .exceptionally(
                ZLinkRedisLocationScriptsClient
                    ::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteStatus>
        removeClientServer(
            ZLinkClientServerServerDescriptorKey key,
            ZLinkLocationOwnerToken owner) {
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeClientServerKey(key);
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.REMOVE_CLIENT_SERVER,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.clientServerDescriptorRowKey(rowKey),
                    keys.clientServerDescriptorMetadataKey(rowKey),
                    keys.clientServerDescriptorIndexKey(),
                    keys.clientServerOwnerTokenIndexKey(
                        owner.ownerId(),
                        owner.leaseGeneration()),
                    keys.clientServerDescriptorGlobalStampKey(),
                    keys.clientServerDescriptorStampKey(
                        key.channelName()),
                    keys.clientServerDescriptorChannelIndexKey(
                        key.channelName())
                },
                owner.ownerId(),
                Long.toString(owner.leaseGeneration()),
                rowKey))
            .thenApply(raw ->
                "stored".equals(string(raw.getFirst()))
                    ? ZLinkLocationWriteStatus.STORED
                    : ZLinkLocationWriteStatus.IGNORED_STALE)
            .exceptionally(
                ZLinkRedisLocationScriptsClient
                    ::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteResult> writeFanoutPublisher(
        systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor
            descriptor,
        ZLinkLocationWriteIntent intent) {
        validateFanoutPublisher(descriptor);
        var key = new systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey(
                descriptor.channelName(),
                descriptor.publisherRid());
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeFanoutPublisherKey(key);
        String json =
            ZLinkLocationDescriptorCodec.serializeFanoutPublisher(
                descriptor);
        if (json.getBytes(StandardCharsets.UTF_8).length
            > 1024 * 1024) {
            throw new IllegalArgumentException(
                "encoded fanout publisher descriptor exceeds 1 MiB");
        }
        String descriptorRow =
            keys.fanoutPublisherDescriptorRowKey(rowKey);
        String metadata =
            keys.fanoutPublisherDescriptorMetadataKey(rowKey);
        return connection.commands()
            .thenCompose(redis -> redis.hmget(
                    metadata,
                    "ownerId",
                    "ownerLeaseGeneration")
                .thenCompose(previous -> {
                    String previousOwner =
                        hashField(previous, "ownerId");
                    String previousLease =
                        hashField(
                            previous,
                            "ownerLeaseGeneration");
                    String placeholder = keys.schemaKey();
                    return redis.<List<Object>>eval(
                        ZLinkRedisLocationScripts
                            .WRITE_FANOUT_PUBLISHER,
                        ScriptOutputType.MULTI,
                        new String[] {
                            descriptorRow,
                            metadata,
                            keys.fanoutPublisherDescriptorIndexKey(),
                            keys.leaseKey(descriptor.ownerId()),
                            keys.counterKey(),
                            keys.fanoutPublisherOwnerTokenIndexKey(
                                descriptor.ownerId(),
                                descriptor.leaseGeneration()),
                            previousOwner == null
                                ? placeholder
                                : keys.leaseKey(previousOwner),
                            previousOwner == null
                                || previousLease == null
                                ? placeholder
                                : keys
                                    .fanoutPublisherOwnerTokenIndexKey(
                                        previousOwner,
                                        Long.parseLong(
                                            previousLease)),
                            keys
                                .fanoutPublisherDescriptorGlobalStampKey(),
                            keys.fanoutPublisherDescriptorStampKey(
                                descriptor.channelName()),
                            keys
                                .fanoutPublisherDescriptorChannelIndexKey(
                                    descriptor.channelName())
                        },
                        intentName(intent),
                        descriptor.ownerId(),
                        Long.toString(
                            descriptor.leaseGeneration()),
                        Long.toString(
                            descriptor.lifecycleGeneration()),
                        Long.toString(
                            descriptor.descriptorRevision()),
                        ZLinkLocationDescriptorCodec
                            .fanoutPublisherImmutableFingerprint(
                                descriptor),
                        json,
                        descriptor.state().name()
                            .toLowerCase(
                                java.util.Locale.ROOT),
                        "",
                        descriptor.channelName(),
                        rowKey,
                        previousOwner == null
                            ? ""
                            : previousOwner,
                        previousLease == null
                            ? ""
                            : previousLease);
                }))
            .thenApply(
                ZLinkRedisLocationScriptsClient::toWriteResult)
            .exceptionally(
                ZLinkRedisLocationScriptsClient
                    ::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteStatus>
        removeFanoutPublisher(
            systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey key,
            ZLinkLocationOwnerToken owner) {
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeFanoutPublisherKey(key);
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.REMOVE_FANOUT_PUBLISHER,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.fanoutPublisherDescriptorRowKey(rowKey),
                    keys.fanoutPublisherDescriptorMetadataKey(rowKey),
                    keys.fanoutPublisherDescriptorIndexKey(),
                    keys.fanoutPublisherOwnerTokenIndexKey(
                        owner.ownerId(),
                        owner.leaseGeneration()),
                    keys
                        .fanoutPublisherDescriptorGlobalStampKey(),
                    keys.fanoutPublisherDescriptorStampKey(
                        key.channelName()),
                    keys
                        .fanoutPublisherDescriptorChannelIndexKey(
                            key.channelName())
                },
                owner.ownerId(),
                Long.toString(owner.leaseGeneration()),
                rowKey))
            .thenApply(raw ->
                "stored".equals(string(raw.getFirst()))
                    ? ZLinkLocationWriteStatus.STORED
                    : ZLinkLocationWriteStatus.IGNORED_STALE)
            .exceptionally(
                ZLinkRedisLocationScriptsClient
                    ::propagateWriteFailure);
    }

    CompletionStage<Map<String, String>> readMeshNodeHashFields(
        ZLinkMeshNodeDescriptorKey key) {
        String rowKey =
            ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(key);
        return connection.commands()
            .thenCompose(redis -> redis.hgetall(
                keys.rowHashKey("mesh-node", rowKey)))
            .thenApply(Map::copyOf);
    }

    CompletionStage<Long> removeAllByOwnerAsync(
        ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(owner, "owner");
        String ownerId = owner.ownerId();
        return connection.commands()
            .thenCompose(redis -> redis.<Long>eval(
                    ZLinkRedisLocationScripts.REMOVE_ALL_BY_OWNER,
                    ScriptOutputType.INTEGER,
                    new String[] {
                        keys.ownerIndexKeyPrefix("peer") + ownerId,
                        keys.ownerIndexKeyPrefix("spot") + ownerId,
                        keys.ownerIndexKeyPrefix("actor") + ownerId,
                        keys.ownerIndexKeyPrefix("route") + ownerId,
                        keys.kindIndexKey("peer"),
                        keys.kindIndexKey("spot"),
                        keys.kindIndexKey("actor"),
                        keys.kindIndexKey("route"),
                        keys.leaseKey(ownerId)
                    },
                    keys.rowHashKeyPrefix("peer"),
                    keys.rowHashKeyPrefix("spot"),
                    keys.rowHashKeyPrefix("actor"),
                    keys.rowHashKeyPrefix("route"),
                    keys.stampKey("peer", null),
                    keys.stampKey("spot", null),
                    keys.stampKey("actor", null),
                    keys.stampKey("route", null),
                    ownerId,
                    Long.toString(owner.leaseGeneration()))
                .thenCompose(genericRemoved -> {
                    if (genericRemoved < 0) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "Owner cleanup token is stale."));
                    }
                            String ownerIndex =
                                keys.meshNodeOwnerTokenIndexKey(
                                    ownerId,
                                    owner.leaseGeneration());
                            return redis.smembers(ownerIndex)
                        .thenCompose(indexed -> {
                            List<String> members =
                                new ArrayList<>(indexed);
                            List<CompletableFuture<String>> meshes =
                                new ArrayList<>(members.size());
                            List<CompletableFuture<String>> entrySpotIds =
                                new ArrayList<>(members.size());
                            for (String member : members) {
                                String storageId =
                                    keys.meshNodeDescriptorStorageId(
                                        member);
                                meshes.add(redis.hget(
                                        keys.rowHashKeyPrefix(
                                            "mesh-node")
                                            + storageId,
                                        "mesh")
                                    .toCompletableFuture());
                                entrySpotIds.add(redis.hget(
                                        keys
                                            .meshNodeDescriptorMetadataKeyPrefix()
                                            + storageId,
                                        "entrySpotId")
                                    .toCompletableFuture());
                            }
                            List<CompletableFuture<?>> reads =
                                new ArrayList<>();
                            reads.addAll(meshes);
                            reads.addAll(entrySpotIds);
                            return CompletableFuture.allOf(
                                    reads.toArray(
                                        CompletableFuture[]::new))
                                .thenCompose(ignored -> {
                                    List<String> scriptKeys =
                                        new ArrayList<>();
                                    scriptKeys.add(ownerIndex);
                                    scriptKeys.add(
                                        keys.kindIndexKey(
                                            "mesh-node"));
                                    scriptKeys.add(
                                        keys.stampKey(
                                            "mesh-node",
                                            null));
                                    scriptKeys.add(
                                        keys.leaseKey(ownerId));
                                    List<String> arguments =
                                        new ArrayList<>();
                                    arguments.add(ownerId);
                                    arguments.add(
                                        Long.toString(
                                            owner.leaseGeneration()));
                                    for (int index = 0;
                                        index < members.size();
                                        index++) {
                                        String member =
                                            members.get(index);
                                        String storageId =
                                            keys.meshNodeDescriptorStorageId(
                                                member);
                                        scriptKeys.add(
                                            keys.rowHashKeyPrefix(
                                                "mesh-node")
                                                + storageId);
                                        scriptKeys.add(
                                            keys
                                                .meshNodeDescriptorMetadataKeyPrefix()
                                                + storageId);
                                        scriptKeys.add(
                                            keys.stampKey(
                                                "mesh-node",
                                                meshes.get(index)
                                                    .join()));
                                        String entrySpotId =
                                            entrySpotIds.get(index)
                                                .join();
                                        scriptKeys.add(
                                            entrySpotId == null
                                                || entrySpotId.isEmpty()
                                                ? keys.schemaKey()
                                                : keys
                                                    .entrySpotIdentityClaimKey(
                                                        entrySpotId));
                                        arguments.add(member);
                                    }
                                    if (members.isEmpty()) {
                                        return CompletableFuture
                                            .completedFuture(
                                                genericRemoved);
                                    }
                                    return redis.<Long>eval(
                                            ZLinkRedisLocationScripts
                                                .REMOVE_ALL_MESH_NODES,
                                            ScriptOutputType.INTEGER,
                                            scriptKeys.toArray(
                                                String[]::new),
                                            arguments.toArray(
                                                String[]::new))
                                        .thenCompose(meshRemoved ->
                                            meshRemoved < 0
                                                ? CompletableFuture
                                                    .failedFuture(
                                                        new IllegalStateException(
                                                            "Owner cleanup token is stale."))
                                                : CompletableFuture
                                                    .completedFuture(
                                                        genericRemoved
                                                            + meshRemoved));
                                });
                        });
                }))
            .thenCompose(removed ->
                removeAllClientServersByOwner(owner)
                    .thenApply(clientServerRemoved ->
                        removed + clientServerRemoved))
            .thenCompose(removed ->
                removeAllFanoutPublishersByOwner(owner)
                    .thenApply(fanoutRemoved ->
                        removed + fanoutRemoved));
    }

    private CompletionStage<Long> removeAllClientServersByOwner(
        ZLinkLocationOwnerToken owner) {
        return connection.commands()
            .thenCompose(redis -> redis.smembers(
                    keys.clientServerOwnerTokenIndexKey(
                        owner.ownerId(),
                        owner.leaseGeneration()))
                .thenCompose(members -> {
                    CompletionStage<Long> removed =
                        CompletableFuture.completedFuture(0L);
                    for (String member : members) {
                        var key = ZLinkRedisLocationKeyCodec
                            .decodeClientServerKey(member);
                        removed = removed.thenCompose(count ->
                            removeClientServer(key, owner)
                                .thenApply(status ->
                                    status
                                        == ZLinkLocationWriteStatus
                                            .STORED
                                            ? count + 1
                                            : count));
                    }
                    return removed;
                }));
    }

    private CompletionStage<Long> removeAllFanoutPublishersByOwner(
        ZLinkLocationOwnerToken owner) {
        return connection.commands()
            .thenCompose(redis -> redis.smembers(
                    keys.fanoutPublisherOwnerTokenIndexKey(
                        owner.ownerId(),
                        owner.leaseGeneration()))
                .thenCompose(members -> {
                    CompletionStage<Long> removed =
                        CompletableFuture.completedFuture(0L);
                    for (String member : members) {
                        var key = ZLinkRedisLocationKeyCodec
                            .decodeFanoutPublisherKey(member);
                        removed = removed.thenCompose(count ->
                            removeFanoutPublisher(key, owner)
                                .thenApply(status ->
                                    status
                                        == ZLinkLocationWriteStatus
                                            .STORED
                                            ? count + 1
                                            : count));
                    }
                    return removed;
                }));
    }

    CompletionStage<ZLinkOwnerLeaseSnapshot> listOwnerLeasesAsync() {
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.LIST_LEASES,
                ScriptOutputType.MULTI,
                new String[] {keys.leaseIndexKey()},
                keys.legacyLeaseKeyPrefix()))
            .thenApply(this::toLeaseSnapshot);
    }

    CompletionStage<ZLinkLocationWriteResult> write(
        String tag,
        String rowKey,
        String meshName,
        String ownerId,
        long generation,
        String json,
        ZLinkLocationWriteIntent intent) {
        return connection.commands()
            .thenCompose(redis -> redis.hget(
                    keys.rowHashKey(tag, rowKey),
                    "owner")
                .thenCompose(currentOwner ->
                    redis.<List<Object>>eval(
                        ZLinkRedisLocationScripts.WRITE,
                        ScriptOutputType.MULTI,
                        new String[] {
                            keys.rowHashKey(tag, rowKey),
                            keys.generationKey(tag, rowKey),
                            keys.kindIndexKey(tag),
                            keys.leaseKey(currentOwner == null
                                ? ownerId
                                : currentOwner)
                        },
                        intentName(intent),
                        ownerId,
                        Long.toString(generation),
                        json,
                        rowKey,
                        "",
                        keys.ownerIndexKeyPrefix(tag),
                        keys.stampKey(tag, meshName),
                        meshName == null
                            ? ""
                            : keys.stampKey(tag, null),
                        meshName == null ? "0" : "1",
                        meshName == null ? "" : meshName)))
            .thenApply(ZLinkRedisLocationScriptsClient::toWriteResult)
            .exceptionally(ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    CompletionStage<ZLinkLocationWriteResult> remove(
        String tag,
        String rowKey,
        String meshName,
        ZLinkLocationOwnerToken owner) {
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ZLinkRedisLocationScripts.REMOVE,
                ScriptOutputType.MULTI,
                new String[] {keys.rowHashKey(tag, rowKey), keys.kindIndexKey(tag)},
                owner.ownerId(),
                Long.toString(owner.leaseGeneration()),
                rowKey,
                keys.ownerIndexKeyPrefix(tag),
                keys.stampKey(tag, meshName),
                meshName == null ? "" : keys.stampKey(tag, null)))
            .thenApply(ZLinkRedisLocationScriptsClient::toWriteResult)
            .exceptionally(ZLinkRedisLocationScriptsClient::propagateWriteFailure);
    }

    private ZLinkOwnerLeaseSnapshot toLeaseSnapshot(List<Object> raw) {
        Instant storeNow = fromUnixMs(number(raw.getFirst()));
        @SuppressWarnings("unchecked")
        List<Object> entries = (List<Object>) raw.get(1);
        List<ZLinkOwnerLease> leases = new ArrayList<>();
        for (int index = 0; index + 2 < entries.size(); index += 3) {
            String ownerId = string(entries.get(index));
            String value = string(entries.get(index + 1));
            long remainingMs = number(entries.get(index + 2));
            int separator = value.indexOf('|');
            RoutingId nodeRid = RoutingId.fromHex(value.substring(0, separator));
            Instant renewedAt = fromUnixMs(Long.parseLong(value.substring(separator + 1)));
            leases.add(new ZLinkOwnerLease(ownerId, nodeRid, storeNow.plusMillis(remainingMs), renewedAt));
        }
        return new ZLinkOwnerLeaseSnapshot(List.copyOf(leases), storeNow);
    }

    private static ZLinkLocationWriteResult toWriteResult(List<Object> result) {
        String status = string(result.get(0));
        return switch (status) {
            case "stored" -> ZLinkLocationWriteResult.stored(number(result.get(1)), fromUnixMs(number(result.get(2))));
            case "conflict" -> ZLinkLocationWriteResult.rejectedConflict();
            case "protocol-error" -> throw new IllegalStateException(
                "same descriptor revision has different bytes");
            default -> ZLinkLocationWriteResult.ignoredStale();
        };
    }

    private static <T> T propagateWriteFailure(Throwable failure) {
        Throwable unwrapped = unwrap(failure);
        if (unwrapped instanceof RedisException
            || unwrapped instanceof RedisConnectionException
            || unwrapped instanceof RedisCommandTimeoutException
            || unwrapped instanceof RedisCommandExecutionException) {
            throw new CompletionException(unwrapped);
        }
        throw new CompletionException(unwrapped);
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof CompletionException || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static String intentName(ZLinkLocationWriteIntent intent) {
        return switch (intent) {
            case NEW_CLAIM -> "new";
            case RENEW -> "renew";
            case TAKEOVER -> "takeover";
        };
    }

    private static void validateFanoutPublisher(
        systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor
            descriptor) {
        if (descriptor.channelName() == null
            || descriptor.channelName().isBlank()
            || descriptor.publisherRid() == null
            || descriptor.endpoint() == null
            || descriptor.endpoint().isBlank()
            || descriptor.securityIdentity() == null
            || descriptor.securityIdentity().isBlank()
            || descriptor.ownerId() == null
            || descriptor.ownerId().isBlank()
            || descriptor.lifecycleGeneration() < 1
            || descriptor.descriptorRevision() < 1
            || descriptor.leaseGeneration() < 1) {
            throw new IllegalArgumentException(
                "fanout publisher descriptor is invalid");
        }
    }

    private static String hashField(
        List<KeyValue<String, String>> fields,
        String name) {
        return fields.stream()
            .filter(field -> name.equals(field.getKey()))
            .findFirst()
            .filter(KeyValue::hasValue)
            .map(KeyValue::getValue)
            .orElse(null);
    }

    private static long number(Object value) {
        if (value instanceof Number number) {
            return number.longValue();
        }
        return Long.parseLong(string(value));
    }

    private static String string(Object value) {
        return value == null ? "" : value.toString();
    }

    private static Instant fromUnixMs(long unixMs) {
        return Instant.ofEpochMilli(unixMs);
    }
}
