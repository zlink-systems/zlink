package systems.zlink.framework.locations.redis;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.lettuce.core.ScriptOutputType;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

final class ZLinkRedisAuthorityClient {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkRedisAuthorityClient.class.getName());
    private static final boolean TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Duration AGGREGATE_COMMIT_RETRY_WINDOW =
        Duration.ofSeconds(5);
    private static final int AGGREGATE_COMMIT_RETRY_LIMIT = 64;
    private static final String PROLOGUE = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local function leaseIsLive(leaseKey, ownerId, generation)
            return redis.call('HGET', leaseKey, 'ownerId')
                    == ownerId
                and redis.call('HGET', leaseKey, 'generation')
                    == generation
                and tonumber(
                    redis.call('HGET', leaseKey, 'expiresAt') or '0')
                    > nowMs
        end
        local function capacityBucket(
            descriptor, lifecycle, kind, stableType)
            return string.len(descriptor) .. ':' .. descriptor
                .. string.len(lifecycle) .. ':' .. lifecycle
                .. string.len(kind) .. ':' .. kind
                .. string.len(stableType) .. ':' .. stableType
        end
        local function nodeCapacityBucket(descriptor, lifecycle, kind)
            local population = kind == 'actor' and 'actor' or 'spot'
            return string.len(descriptor) .. ':' .. descriptor
                .. string.len(lifecycle) .. ':' .. lifecycle
                .. string.len(population) .. ':' .. population
        end
        local function nodeCapacityKey(capacityKeys, bucket, phase)
            local actor = string.sub(bucket, -7) == '5:actor'
            if actor then
                return phase == 'active'
                    and capacityKeys[3] or capacityKeys[4]
            end
            return phase == 'active'
                and capacityKeys[5] or capacityKeys[6]
        end
        local function capacityValue(
            capacityKeys, bucket, phase, isNode)
            local key = isNode
                and nodeCapacityKey(capacityKeys, bucket, phase)
                or (phase == 'active'
                    and capacityKeys[1] or capacityKeys[2])
            local value = redis.call('HGET', key, bucket)
            if not value then return 0 end
            local number = tonumber(value)
            if not number or number < 0 then return nil end
            return number
        end
        local function canReserve(
            capacityKeys, bucket, nodeBucket, delta,
            typeLimit, populationLimit)
            typeLimit = tonumber(typeLimit)
            populationLimit = tonumber(populationLimit)
            if not typeLimit or typeLimit < 0
                or not populationLimit or populationLimit < 0 then
                return false
            end
            local nodeActive =
                capacityValue(
                    capacityKeys, nodeBucket, 'active', true)
            local nodePending =
                capacityValue(
                    capacityKeys, nodeBucket, 'pending', true)
            if not nodeActive or not nodePending then
                return false
            end
            if bucket == nodeBucket then
                return populationLimit == 0
                    or nodeActive + nodePending + delta
                        <= populationLimit
            end
            local active = capacityValue(
                capacityKeys, bucket, 'active', false)
            local pending = capacityValue(
                capacityKeys, bucket, 'pending', false)
            return active and pending
                and (typeLimit == 0
                    or active + pending + delta <= typeLimit)
                and (populationLimit == 0
                    or nodeActive + nodePending + delta
                        <= populationLimit)
        end
        local function canAdjust(
            capacityKeys, bucket, phase, delta, isNode)
            local value = capacityValue(
                capacityKeys, bucket, phase, isNode)
            return value and value + delta >= 0
        end
        local function canAdjustCapacity(
            capacityKeys, bucket, nodeBucket, phase, delta)
            return canAdjust(
                    capacityKeys, nodeBucket, phase, delta, true)
                and (bucket == nodeBucket
                    or canAdjust(
                        capacityKeys, bucket, phase, delta, false))
        end
        local function adjustCapacity(
            capacityKeys, bucket, nodeBucket, phase, delta)
            if not canAdjust(
                    capacityKeys, nodeBucket, phase, delta, true)
                or (bucket ~= nodeBucket
                    and not canAdjust(
                        capacityKeys, bucket, phase, delta, false)) then
                return false
            end
            if bucket ~= nodeBucket then
                local typeKey = phase == 'active'
                    and capacityKeys[1] or capacityKeys[2]
                local value = redis.call(
                    'HINCRBY', typeKey, bucket, delta)
                if value == 0 then
                    redis.call('HDEL', typeKey, bucket)
                end
            end
            local nodeKey = nodeCapacityKey(
                capacityKeys, nodeBucket, phase)
            local nodeValue = redis.call(
                'HINCRBY',
                nodeKey,
                nodeBucket,
                delta)
            if nodeValue == 0 then
                redis.call(
                    'HDEL',
                    nodeKey,
                    nodeBucket)
            end
            return true
        end
        local function readBundleSegment(value, offset)
            local colon = string.find(value, ':', offset, true)
            if not colon then return nil end
            local length = tonumber(string.sub(value, offset, colon - 1))
            if not length or length < 0 then return nil end
            local first = colon + 1
            local last = first + length - 1
            if last > string.len(value) then return nil end
            return string.sub(value, first, last), last + 1
        end
        local function capacityBundle(value)
            local domain, offset = readBundleSegment(value or '', 1)
            local actors
            actors, offset = readBundleSegment(value or '', offset or 1)
            local spots
            spots, offset = readBundleSegment(value or '', offset or 1)
            local presence
            presence, offset = readBundleSegment(value or '', offset or 1)
            if domain ~= 'zlink-capacity-bundle-v2'
                or not actors or not spots or not presence then
                return nil
            end
            actors = tonumber(actors)
            spots = tonumber(spots)
            if not actors or actors < 0 or not spots or spots < 0 then
                return nil
            end
            if presence == '0' and offset == string.len(value) + 1 then
                return actors, spots, nil, nil, 0
            end
            if presence ~= '1' then return nil end
            local kind
            kind, offset = readBundleSegment(value, offset)
            local stableType
            stableType, offset = readBundleSegment(value, offset or 1)
            local slots
            slots, offset = readBundleSegment(value, offset or 1)
            slots = tonumber(slots)
            if not kind or not stableType or not slots or slots <= 0
                or offset ~= string.len(value) + 1 then
                return nil
            end
            return actors, spots, kind, stableType, slots
        end
        local function capacityDelta(value, kind)
            local actors, spots, spotKind, stableType, slots =
                capacityBundle(value)
            if not actors then return nil end
            if kind == 'actor' then
                if actors <= 0 or spots ~= 0 or spotKind then return nil end
                return actors
            end
            if actors ~= 0 or spots <= 0
                or spotKind ~= kind or slots ~= spots then
                return nil
            end
            return spots
        end
        local function bundleCapacity(
            descriptor, lifecycle, value)
            local actors, spots, spotKind, stableType, typeSlots =
                capacityBundle(value)
            if not actors then return nil end
            local actorBucket = nodeCapacityBucket(
                descriptor, lifecycle, 'actor')
            local spotNodeBucket = nodeCapacityBucket(
                descriptor, lifecycle, 'user_spot')
            local typeBucket = nil
            if spots > 0 then
                typeBucket = capacityBucket(
                    descriptor, lifecycle, spotKind, stableType)
            end
            return actors, spots, spotKind, stableType, typeSlots,
                actorBucket, spotNodeBucket, typeBucket
        end
        local function canAdjustBundle(
            capacityKeys, descriptor, lifecycle, value, phase, direction)
            local actors, spots, _, _, _, actorBucket,
                spotNodeBucket, typeBucket =
                bundleCapacity(descriptor, lifecycle, value)
            if not actors then return false end
            return (actors == 0 or canAdjustCapacity(
                    capacityKeys, actorBucket, actorBucket,
                    phase, direction * actors))
                and (spots == 0 or canAdjustCapacity(
                    capacityKeys, typeBucket, spotNodeBucket,
                    phase, direction * spots))
        end
        local function adjustBundle(
            capacityKeys, descriptor, lifecycle, value, phase, direction)
            local actors, spots, _, _, _, actorBucket,
                spotNodeBucket, typeBucket =
                bundleCapacity(descriptor, lifecycle, value)
            if not actors then return false end
            if actors > 0 then
                adjustCapacity(
                    capacityKeys, actorBucket, actorBucket,
                    phase, direction * actors)
            end
            if spots > 0 then
                adjustCapacity(
                    capacityKeys, typeBucket, spotNodeBucket,
                    phase, direction * spots)
            end
            return true
        end
        local function descriptorAcceptsBundle(
            descriptorKey, admissionKey,
            descriptorIdentity, lifecycle,
            ownerId, leaseGeneration, capacityKeys, value)
            local public = redis.call(
                'HMGET', descriptorKey, 'owner', 'gen')
            local metadata = redis.call(
                'HMGET', admissionKey,
                'descriptorKey', 'lifecycleGeneration',
                'ownerId', 'ownerLeaseGeneration',
                'objectRole', 'runtimeState', 'capabilities',
                'actorLimit', 'spotLimit')
            if not metadata[1]
                or public[1] ~= ownerId or public[2] ~= lifecycle
                or metadata[1] ~= descriptorIdentity
                or metadata[2] ~= lifecycle
                or metadata[3] ~= ownerId
                or metadata[4] ~= leaseGeneration
                or metadata[5] ~= 'server'
                or metadata[6] ~= '1' then
                return false
            end
            local actors, spots, spotKind, stableType, _,
                actorBucket, spotNodeBucket, typeBucket =
                bundleCapacity(descriptorIdentity, lifecycle, value)
            local actorLimit = tonumber(metadata[8])
            local spotLimit = tonumber(metadata[9])
            if not actors or not actorLimit or not spotLimit then
                return false
            end
            if actors > 0 and not canReserve(
                    capacityKeys, actorBucket, actorBucket, actors,
                    0, actorLimit) then
                return false
            end
            if spots == 0 then return true end
            local ok, capabilities = pcall(
                cjson.decode, metadata[7] or '')
            if not ok or type(capabilities) ~= 'table' then
                return false
            end
            local typeLimit = nil
            for _, candidate in ipairs(capabilities) do
                local candidateKind =
                    string.lower(tostring(candidate.objectKind or ''))
                if candidateKind == '2' then
                    candidateKind = 'user_spot'
                elseif candidateKind == '3' then
                    candidateKind = 'instance_spot'
                end
                if candidateKind == spotKind
                    and tostring(candidate.stableType or '')
                        == stableType then
                    typeLimit = tonumber(candidate.spotLimit)
                    break
                end
            end
            return typeLimit
                and canReserve(
                    capacityKeys, typeBucket, spotNodeBucket, spots,
                    typeLimit, spotLimit)
        end
        local function canonicalObjectKind(value)
            local text = string.lower(tostring(value or ''))
            if text == '1' or text == 'actor' then return 'actor' end
            if text == '2' or text == 'user_spot' then
                return 'user_spot'
            end
            if text == '3' or text == 'instance_spot' then
                return 'instance_spot'
            end
            return nil
        end
        local function descriptorAccepts(
            descriptorKey, admissionKey,
            descriptorIdentity, lifecycle,
            ownerId, leaseGeneration, kind, stableType,
            capacityKeys, delta)
            if not delta or delta < 0 then
                return false, 'unavailable'
            end
            local public = redis.call(
                'HMGET', descriptorKey, 'owner', 'gen')
            local metadata = redis.call(
                'HMGET', admissionKey,
                'descriptorKey', 'lifecycleGeneration',
                'ownerId', 'ownerLeaseGeneration',
                'objectRole', 'runtimeState', 'capabilities',
                'actorLimit', 'spotLimit')
            if not metadata[1]
                or public[1] ~= ownerId
                or public[2] ~= lifecycle
                or metadata[1] ~= descriptorIdentity
                or metadata[2] ~= lifecycle
                or metadata[3] ~= ownerId
                or metadata[4] ~= leaseGeneration
                or metadata[5] ~= 'server'
                or metadata[6] ~= '1' then
                return false, 'unavailable'
            end
            local ok, capabilities = pcall(
                cjson.decode, metadata[7] or '')
            if not ok or type(capabilities) ~= 'table' then
                return false, 'unavailable'
            end
            local capability = nil
            for _, candidate in ipairs(capabilities) do
                if canonicalObjectKind(candidate.objectKind) == kind
                    and tostring(candidate.stableType or '')
                        == stableType then
                    capability = candidate
                    break
                end
            end
            if not capability then
                return false, 'unavailable'
            end
            local actorLimit = tonumber(metadata[8])
            local spotLimit = tonumber(metadata[9])
            local typeLimit = kind == 'actor'
                and 0 or tonumber(capability.spotLimit)
            local populationLimit = kind == 'actor'
                and actorLimit or spotLimit
            if not actorLimit or not spotLimit
                or not typeLimit or not populationLimit then
                return false, 'unavailable'
            end
            local bucket = capacityBucket(
                descriptorIdentity, lifecycle, kind, stableType)
            local nodeBucket = nodeCapacityBucket(
                descriptorIdentity, lifecycle, kind)
            if kind == 'actor' then bucket = nodeBucket end
            if not canReserve(
                capacityKeys, bucket, nodeBucket, delta,
                typeLimit, populationLimit) then
                return false, 'capacity'
            end
            return true, bucket, nodeBucket
        end
        local maxCounter = '9223372036854775807'
        local function counterValue(counterKey, field)
            return redis.call('HGET', counterKey, field) or '0'
        end
        local function incrementCounter(counterKey, field)
            return redis.call('HINCRBY', counterKey, field, 1)
        end
        local function normalizeDecimal(value)
            value = string.gsub(value or '0', '^0+', '')
            return value == '' and '0' or value
        end
        local function compareDecimal(left, right)
            left = normalizeDecimal(left)
            right = normalizeDecimal(right)
            if string.len(left) < string.len(right) then return -1 end
            if string.len(left) > string.len(right) then return 1 end
            if left < right then return -1 end
            if left > right then return 1 end
            return 0
        end
        local function addSmallDecimal(value, delta)
            value = normalizeDecimal(value)
            local digits = {}
            local carry = delta
            for index = string.len(value), 1, -1 do
                local digit = string.byte(value, index) - 48
                local sum = digit + (carry % 10)
                carry = math.floor(carry / 10)
                if sum >= 10 then
                    sum = sum - 10
                    carry = carry + 1
                end
                table.insert(
                    digits, 1, string.char(sum + 48))
            end
            while carry > 0 do
                table.insert(
                    digits, 1,
                    string.char((carry % 10) + 48))
                carry = math.floor(carry / 10)
            end
            return table.concat(digits)
        end
        local function canIncrementBy(value, count)
            return compareDecimal(
                addSmallDecimal(value or '0', count),
                maxCounter) <= 0
        end
        local function decimalToHex16(value)
            value = normalizeDecimal(value)
            local hex = ''
            repeat
                local quotient = ''
                local remainder = 0
                for index = 1, string.len(value) do
                    local current = remainder * 10
                        + string.byte(value, index) - 48
                    local digit = math.floor(current / 16)
                    remainder = current % 16
                    if quotient ~= '' or digit ~= 0 then
                        quotient = quotient .. tostring(digit)
                    end
                end
                hex = string.sub(
                    '0123456789abcdef',
                    remainder + 1,
                    remainder + 1) .. hex
                value = quotient == '' and '0' or quotient
            until value == '0'
            return string.rep('0', 16 - string.len(hex)) .. hex
        end
        local historyFields = {
            'storeVersion', 'payload', 'objectGeneration',
            'authorityOwnerGeneration', 'ownerId',
            'ownerLeaseGeneration', 'allocationState',
            'objectKind', 'stableType',
            'descriptorKey',
            'descriptorLifecycleGeneration',
            'capacityBundle', 'authorityKey'
        }
        local function pruneHistory(
            history, revisions, watermarks, expiry)
            local expired = redis.call(
                'ZRANGEBYSCORE', expiry, '-inf', nowMs,
                'LIMIT', 0, 32)
            for _, scan in ipairs(expired) do
                redis.call('ZREM', expiry, scan)
                redis.call('ZREM', watermarks, scan)
            end
            local oldest = redis.call(
                'ZRANGE', watermarks, 0, 0)
            local keep = nil
            if oldest[1] then
                keep = redis.call(
                    'ZREVRANGEBYLEX', revisions,
                    '[' .. string.sub(oldest[1], 1, 16),
                    '-', 'LIMIT', 0, 1)[1]
            else
                keep = redis.call(
                    'ZREVRANGE', revisions, 0, 0)[1]
            end
            if not keep then return end
            local retired = redis.call(
                'ZRANGEBYLEX', revisions,
                '-', '(' .. keep, 'LIMIT', 0, 32)
            for _, member in ipairs(retired) do
                for _, field in ipairs(historyFields) do
                    redis.call('HDEL', history,
                        member .. ':' .. field)
                end
                redis.call('HDEL', history, member .. ':deleted')
                redis.call('ZREM', revisions, member)
            end
        end
        local function recordHistory(
            row, history, revisions, watermarks, expiry)
            local revision = redis.call('HGET', row, 'storeVersion')
            local member = decimalToHex16(revision)
            redis.call('HSET', history, member .. ':deleted', '0')
            for _, field in ipairs(historyFields) do
                redis.call('HSET', history,
                    member .. ':' .. field,
                    redis.call('HGET', row, field) or '')
            end
            redis.call('ZADD', revisions, 0, member)
            pruneHistory(history, revisions, watermarks, expiry)
        end
        local function recordTombstone(
            history, revisions, watermarks,
            expiry, indexGc, revision, authorityKey, encodedKey)
            local member = decimalToHex16(revision)
            redis.call('HSET', history,
                member .. ':deleted', '1',
                member .. ':authorityKey', authorityKey)
            redis.call('ZADD', revisions, 0, member)
            redis.call('ZADD', indexGc, 0,
                member .. ':' .. encodedKey)
            pruneHistory(history, revisions, watermarks, expiry)
        end
        local function recordMembership(
            history, revisions, revision, mutation)
            local member = decimalToHex16(revision)
            redis.call('HSET', history, member, mutation)
            redis.call('ZADD', revisions, 0, member)
        end
        """;
    private static final String READ = PROLOGUE + """
        if redis.call('EXISTS', KEYS[1]) == 0 then
            return {'missing', nowMs}
        end
        return {'found', nowMs,
            redis.call('HGET', KEYS[1], 'storeVersion'),
            redis.call('HGET', KEYS[1], 'payload'),
            redis.call('HGET', KEYS[1], 'objectGeneration'),
            redis.call('HGET', KEYS[1], 'authorityOwnerGeneration'),
            redis.call('HGET', KEYS[1], 'ownerId'),
            redis.call('HGET', KEYS[1], 'ownerLeaseGeneration'),
            redis.call('HGET', KEYS[1], 'allocationState'),
            redis.call('HGET', KEYS[1], 'objectKind'),
            redis.call('HGET', KEYS[1], 'stableType'),
            redis.call('HGET', KEYS[1], 'descriptorKey'),
            redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration'),
            redis.call('HGET', KEYS[1], 'capacityBundle'),
            redis.call('HGET', KEYS[1], 'creationReservationId'),
            redis.call('HGET', KEYS[1], 'creationIntentReference'),
            redis.call('HGET', KEYS[1], 'creationIntentSha256'),
            redis.call('HGET', KEYS[1], 'creationIntentEncodedSize')}
        """;
    private static final String CAS = PROLOGUE + """
        local capacityKeys = {
            KEYS[8], KEYS[13], KEYS[14], KEYS[15],
            KEYS[19], KEYS[20]}
        local exists = redis.call('EXISTS', KEYS[1]) == 1
        if not exists
            or redis.call('HGET', KEYS[1], 'storeVersion') ~= ARGV[2]
            or redis.call('HGET', KEYS[1], 'allocationState') ~= 'active' then
            return {'conflict', nowMs}
        end
        if ARGV[3] == 'delete' then
            local currentOwner = redis.call('HGET', KEYS[1], 'ownerId')
            local currentLease =
                redis.call('HGET', KEYS[1], 'ownerLeaseGeneration')
            if not leaseIsLive(
                KEYS[4], currentOwner, currentLease) then
                return {'conflict', nowMs}
            end
            if counterValue(KEYS[3], 'storeRevision')
                    == maxCounter then
                return {'generation-exhausted', nowMs}
            end
            local descriptor = redis.call(
                'HGET', KEYS[1], 'descriptorKey')
            local lifecycle = redis.call(
                'HGET', KEYS[1], 'descriptorLifecycleGeneration')
            local kind = redis.call('HGET', KEYS[1], 'objectKind')
            local stableType = redis.call(
                'HGET', KEYS[1], 'stableType')
            local bucket = capacityBucket(
                descriptor, lifecycle, kind, stableType)
            local nodeBucket = nodeCapacityBucket(
                descriptor, lifecycle, kind)
            if kind == 'actor' then bucket = nodeBucket end
            local delta = capacityDelta(
                redis.call('HGET', KEYS[1], 'capacityBundle'),
                kind)
            if not bucket or not nodeBucket or not delta
                or not canAdjustCapacity(
                    capacityKeys, bucket, nodeBucket,
                    'active', -delta) then
                return {'conflict', nowMs}
            end
            local revision = incrementCounter(
                KEYS[3], 'storeRevision')
            recordTombstone(
                KEYS[11], KEYS[12], KEYS[16],
                KEYS[18], KEYS[17],
                revision, ARGV[10], ARGV[6])
            adjustCapacity(
                capacityKeys, bucket, nodeBucket, 'active', -delta)
            redis.call('DEL', KEYS[1])
            return {'deleted', nowMs, revision}
        end
        if ARGV[3] == 'restore' then
            if redis.call('HGET', KEYS[1], 'ownerId') ~= ARGV[7]
                or redis.call(
                    'HGET', KEYS[1], 'ownerLeaseGeneration') ~= ARGV[8] then
                return {'conflict', nowMs}
            end
            if counterValue(KEYS[3], 'storeRevision') == maxCounter then
                return {'generation-exhausted', nowMs}
            end
            local revision = incrementCounter(
                KEYS[3], 'storeRevision')
            redis.call('HSET', KEYS[1],
                'storeVersion', revision,
                'payload', ARGV[5],
                'authorityKey', ARGV[10])
            recordHistory(
                KEYS[1], KEYS[11], KEYS[12],
                KEYS[16], KEYS[18])
            redis.call('ZADD', KEYS[2], 0, ARGV[6])
            return {'stored', nowMs, revision,
                redis.call('HGET', KEYS[1], 'objectGeneration'),
                redis.call(
                    'HGET', KEYS[1], 'authorityOwnerGeneration'),
                redis.call('HGET', KEYS[1], 'ownerId'),
                redis.call('HGET', KEYS[1], 'ownerLeaseGeneration'),
                redis.call('HGET', KEYS[1], 'allocationState'),
                redis.call('HGET', KEYS[1], 'objectKind'),
                redis.call('HGET', KEYS[1], 'stableType'),
                redis.call('HGET', KEYS[1], 'descriptorKey'),
                redis.call(
                    'HGET', KEYS[1], 'descriptorLifecycleGeneration'),
                redis.call('HGET', KEYS[1], 'capacityBundle')}
        end
        local targetOwner = ARGV[7]
        local targetLease = ARGV[8]
        if ARGV[4] == 'preserve' then
            targetOwner = redis.call('HGET', KEYS[1], 'ownerId')
            targetLease =
                redis.call('HGET', KEYS[1], 'ownerLeaseGeneration')
        end
        if not leaseIsLive(
            KEYS[4], targetOwner, targetLease) then
            return {'conflict', nowMs}
        end
        local targetDescriptor = nil
        local targetLifecycle = nil
        local targetBucket = nil
        local targetNodeBucket = nil
        local sourceBucket = nil
        local sourceNodeBucket = nil
        local delta = nil
        if ARGV[4] == 'new-owner' then
            local currentVersion =
                redis.call('HGET', KEYS[1], 'storeVersion')
            local currentOwner =
                redis.call('HGET', KEYS[1], 'ownerId')
            local currentLease =
                redis.call('HGET', KEYS[1], 'ownerLeaseGeneration')
            if redis.call('HGET', KEYS[7], 'state')
                    ~= 'reserved'
                or redis.call(
                    'HGET', KEYS[7], 'authorityKey')
                    ~= ARGV[10]
                or redis.call(
                    'HGET', KEYS[7], 'expectedVersion')
                    ~= currentVersion
                or redis.call(
                    'HGET', KEYS[7], 'sourceOwner')
                    ~= currentOwner
                or redis.call(
                    'HGET', KEYS[7], 'sourceLease')
                    ~= currentLease
                or redis.call(
                    'HGET', KEYS[7], 'targetOwner')
                    ~= targetOwner
                or redis.call(
                    'HGET', KEYS[7], 'targetLease')
                    ~= targetLease
                or redis.call('HGET', KEYS[7], 'kind')
                    ~= redis.call(
                        'HGET', KEYS[1], 'objectKind')
                or redis.call(
                    'HGET', KEYS[7], 'stableType')
                    ~= redis.call(
                        'HGET', KEYS[1], 'stableType')
                or redis.call(
                    'HGET', KEYS[7], 'sourceDescriptor')
                    ~= redis.call(
                        'HGET', KEYS[1], 'descriptorKey')
                or redis.call(
                    'HGET', KEYS[7], 'sourceLifecycle')
                    ~= redis.call(
                        'HGET', KEYS[1],
                        'descriptorLifecycleGeneration')
                or redis.call('HGET', KEYS[7], 'capacityBundle')
                    ~= redis.call(
                        'HGET', KEYS[1],
                        'capacityBundle') then
                return {'conflict', nowMs}
            end
            targetDescriptor = redis.call(
                'HGET', KEYS[7], 'descriptorKey')
            targetLifecycle = redis.call(
                'HGET', KEYS[7], 'targetLifecycle')
            targetBucket = redis.call(
                'HGET', KEYS[7], 'targetBucket')
            targetNodeBucket = redis.call(
                'HGET', KEYS[7], 'targetNodeBucket')
            sourceBucket = redis.call(
                'HGET', KEYS[7], 'sourceBucket')
            sourceNodeBucket = redis.call(
                'HGET', KEYS[7], 'sourceNodeBucket')
            delta = capacityDelta(
                redis.call('HGET', KEYS[7], 'capacityBundle'),
                redis.call('HGET', KEYS[7], 'kind'))
            local descriptorRowKey = KEYS[9]
            local accepted, checkedBucket, checkedNodeBucket =
                descriptorAccepts(
                    descriptorRowKey, KEYS[10],
                    targetDescriptor,
                    targetLifecycle, targetOwner, targetLease,
                    redis.call('HGET', KEYS[7], 'kind'),
                    redis.call('HGET', KEYS[7], 'stableType'),
                    capacityKeys, 0)
            local currentSourceBucket = capacityBucket(
                redis.call('HGET', KEYS[1], 'descriptorKey'),
                redis.call(
                    'HGET', KEYS[1],
                    'descriptorLifecycleGeneration'),
                redis.call('HGET', KEYS[1], 'objectKind'),
                redis.call('HGET', KEYS[1], 'stableType'))
            local currentSourceNodeBucket = nodeCapacityBucket(
                redis.call('HGET', KEYS[1], 'descriptorKey'),
                redis.call(
                    'HGET', KEYS[1],
                    'descriptorLifecycleGeneration'),
                redis.call('HGET', KEYS[1], 'objectKind'))
            if redis.call('HGET', KEYS[1], 'objectKind') == 'actor' then
                currentSourceBucket = currentSourceNodeBucket
            end
            if not accepted
                or checkedBucket ~= targetBucket
                or checkedNodeBucket ~= targetNodeBucket
                or not delta
                or sourceBucket ~= currentSourceBucket
                or sourceNodeBucket ~= currentSourceNodeBucket
                or not canAdjustCapacity(
                    capacityKeys, sourceBucket, sourceNodeBucket,
                    'active', -delta)
                or not canAdjustCapacity(
                    capacityKeys, targetBucket, targetNodeBucket,
                    'pending', -delta)
                or not canAdjustCapacity(
                    capacityKeys, targetBucket, targetNodeBucket,
                    'active', delta) then
                return {'conflict', nowMs}
            end
        end
        if counterValue(KEYS[3], 'storeRevision') == maxCounter
            or (ARGV[4] ~= 'preserve'
                and counterValue(
                    KEYS[6], 'authorityOwnerGeneration')
                    == maxCounter) then
            return {'generation-exhausted', nowMs}
        end
        local revision = incrementCounter(
            KEYS[3], 'storeRevision')
        local objectGeneration =
            redis.call('HGET', KEYS[1], 'objectGeneration')
        local ownerGeneration = ARGV[4] == 'preserve'
            and redis.call('HGET', KEYS[1], 'authorityOwnerGeneration')
            or incrementCounter(
                KEYS[6], 'authorityOwnerGeneration')
        if ARGV[4] == 'new-owner' then
            adjustCapacity(
                capacityKeys, sourceBucket, sourceNodeBucket,
                'active', -delta)
            adjustCapacity(
                capacityKeys, targetBucket, targetNodeBucket,
                'pending', -delta)
            adjustCapacity(
                capacityKeys, targetBucket, targetNodeBucket,
                'active', delta)
        end
        redis.call('HSET', KEYS[1],
            'storeVersion', revision,
            'payload', ARGV[5],
            'objectGeneration', objectGeneration,
            'authorityOwnerGeneration', ownerGeneration,
            'ownerId', targetOwner,
            'ownerLeaseGeneration', targetLease,
            'authorityKey', ARGV[10])
        if ARGV[4] == 'new-owner' then
            redis.call('HSET', KEYS[1],
                'descriptorKey', targetDescriptor,
                'descriptorLifecycleGeneration',
                    targetLifecycle)
        end
        recordHistory(
            KEYS[1], KEYS[11], KEYS[12],
            KEYS[16], KEYS[18])
        redis.call('ZADD', KEYS[2], 0, ARGV[6])
        if ARGV[4] == 'new-owner' then
            redis.call('HSET', KEYS[7],
                'state', 'committed',
                'committedVersion', revision)
        end
        return {'stored', nowMs, revision,
            objectGeneration, ownerGeneration, targetOwner, targetLease,
            redis.call('HGET', KEYS[1], 'allocationState'),
            redis.call('HGET', KEYS[1], 'objectKind'),
            redis.call('HGET', KEYS[1], 'stableType'),
            redis.call('HGET', KEYS[1], 'descriptorKey'),
            redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration'),
            redis.call('HGET', KEYS[1], 'capacityBundle')}
        """;
    private static final String RESERVE = PROLOGUE + """
        local capacityKeys = {
            KEYS[7], KEYS[12], KEYS[13], KEYS[14],
            KEYS[19], KEYS[20]}
        if redis.call('EXISTS', KEYS[1]) == 1 then
            local status = redis.call('HGET', KEYS[1], 'stableType') ~= ARGV[2]
                and 'type-mismatch'
                or redis.call('HGET', KEYS[1], 'allocationState') == 'pending'
                    and 'creating-conflict'
                    or 'already-exists'
            return {status, nowMs,
                redis.call('HGET', KEYS[1], 'storeVersion'),
                redis.call('HGET', KEYS[1], 'payload'),
                redis.call('HGET', KEYS[1], 'objectGeneration'),
                redis.call('HGET', KEYS[1], 'authorityOwnerGeneration'),
                redis.call('HGET', KEYS[1], 'ownerId'),
                redis.call('HGET', KEYS[1], 'ownerLeaseGeneration'),
                redis.call('HGET', KEYS[1], 'allocationState'),
                redis.call('HGET', KEYS[1], 'objectKind'),
                redis.call('HGET', KEYS[1], 'stableType'),
                redis.call('HGET', KEYS[1], 'descriptorKey'),
                redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration'),
                redis.call('HGET', KEYS[1], 'capacityBundle'),
                redis.call('HGET', KEYS[1], 'creationReservationId'),
                redis.call('HGET', KEYS[1], 'creationIntentReference'),
                redis.call('HGET', KEYS[1], 'creationIntentSha256'),
                redis.call('HGET', KEYS[1], 'creationIntentEncodedSize')}
        end
        if ARGV[11] ~= 'actor'
            and redis.call('EXISTS', KEYS[18]) == 1 then
            return {'identity-conflict', nowMs}
        end
        if not leaseIsLive(KEYS[6], ARGV[3], ARGV[4]) then
            return {'owner-stale', nowMs}
        end
        if counterValue(KEYS[4], 'objectGeneration') == maxCounter
            or counterValue(
                KEYS[5], 'authorityOwnerGeneration') == maxCounter
            or counterValue(KEYS[3], 'storeRevision') == maxCounter then
            return {'generation-exhausted', nowMs}
        end
        local accepted, reason, nodeBucket = descriptorAccepts(
            KEYS[8], KEYS[9],
            ARGV[9], ARGV[10], ARGV[3], ARGV[4],
            ARGV[11], ARGV[2], capacityKeys,
            capacityDelta(ARGV[12], ARGV[11]))
        if not accepted then
            return {
                reason == 'capacity'
                    and 'capacity-exhausted'
                    or 'owner-stale',
                nowMs}
        end
        if not adjustCapacity(
            capacityKeys, reason, nodeBucket, 'pending',
            capacityDelta(ARGV[12], ARGV[11])) then
            return {'capacity-exhausted', nowMs}
        end
        local objectGeneration = incrementCounter(
            KEYS[4], 'objectGeneration')
        local ownerGeneration = incrementCounter(
            KEYS[5], 'authorityOwnerGeneration')
        local revision = incrementCounter(
            KEYS[3], 'storeRevision')
        local reservationVersion = ARGV[13]
        redis.call('HSET', KEYS[1],
            'storeVersion', revision,
            'payload', ARGV[8],
            'objectGeneration', objectGeneration,
            'authorityOwnerGeneration', ownerGeneration,
            'ownerId', ARGV[3],
            'ownerLeaseGeneration', ARGV[4],
            'stableType', ARGV[2],
            'authorityKey', ARGV[7],
            'descriptorKey', ARGV[9],
            'descriptorLifecycleGeneration', ARGV[10],
            'allocationState', 'pending',
            'objectKind', ARGV[11],
            'capacityBundle', ARGV[12],
            'creationReservationId', reservationVersion,
            'creationIntentReference', ARGV[5],
            'creationIntentSha256', ARGV[6],
            'creationIntentEncodedSize', ARGV[14])
        redis.call('HSET', KEYS[15],
            'state', 'reserved',
            'reservationId', reservationVersion,
            'authorityKey', ARGV[7],
            'storeVersion', revision,
            'objectGeneration', objectGeneration,
            'authorityOwnerGeneration', ownerGeneration,
            'ownerId', ARGV[3],
            'ownerLeaseGeneration', ARGV[4],
            'creationIntentReference', ARGV[5],
            'creationIntentHash', ARGV[6],
            'descriptorKey', ARGV[9],
            'descriptorLifecycleGeneration', ARGV[10],
            'capacityBundle', ARGV[12])
        recordHistory(
            KEYS[1], KEYS[10], KEYS[11],
            KEYS[16], KEYS[17])
        redis.call('ZADD', KEYS[2], 0, ARGV[1])
        return {'reserved', nowMs, revision, objectGeneration,
            ownerGeneration, reservationVersion}
        """;
    private static final String COMMIT_RESERVATION = PROLOGUE + """
        local capacityKeys = {
            KEYS[4], KEYS[9], KEYS[10], KEYS[11],
            KEYS[17], KEYS[18]}
        if ARGV[9] ~= '' then
            if tonumber(ARGV[12]) <= nowMs then
                return {'invalid-expiry', nowMs}
            end
            if redis.call('EXISTS', KEYS[16]) == 1 then
                if redis.call('HGET', KEYS[16], 'reservationId') == ARGV[1]
                    and redis.call('HGET', KEYS[16], 'state') == ARGV[9]
                    and redis.call('HGET', KEYS[16], 'terminalSha256') == ARGV[11]
                    and redis.call('HGET', KEYS[16], 'expiresAtMs') == ARGV[12]
                    and redis.call('HGET', KEYS[16], 'authorityKey') == ARGV[13]
                    and redis.call('HGET', KEYS[16], 'storeVersion') == ARGV[14] then
                    return {'already-committed', nowMs}
                end
                return {'stale', nowMs}
            end
        end
        if redis.call('EXISTS', KEYS[1]) == 0 then
            return {'stale', nowMs}
        end
        if redis.call('EXISTS', KEYS[12]) == 0 then
            if redis.call('HGET', KEYS[1], 'allocationState') == 'active'
                and redis.call('HGET', KEYS[1], 'objectGeneration') == ARGV[2]
                and redis.call('HGET', KEYS[1], 'authorityOwnerGeneration') == ARGV[3]
                and redis.call('HGET', KEYS[1], 'ownerId') == ARGV[4]
                and redis.call('HGET', KEYS[1], 'ownerLeaseGeneration') == ARGV[5]
                and redis.call('HGET', KEYS[1], 'descriptorKey') == ARGV[6]
                and redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration') == ARGV[7] then
                return {'already-committed', nowMs}
            end
            return {'stale', nowMs}
        end
        if redis.call(
                'HGET', KEYS[12], 'reservationId') ~= ARGV[1]
            or redis.call('HGET', KEYS[1], 'objectGeneration') ~= ARGV[2]
            or redis.call('HGET', KEYS[1], 'authorityOwnerGeneration') ~= ARGV[3]
            or redis.call('HGET', KEYS[1], 'ownerId') ~= ARGV[4]
            or redis.call('HGET', KEYS[1], 'ownerLeaseGeneration') ~= ARGV[5]
            or redis.call('HGET', KEYS[1], 'descriptorKey') ~= ARGV[6]
            or redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration') ~= ARGV[7] then
            return {'stale', nowMs}
        end
        if redis.call('HGET', KEYS[12], 'state') ~= 'reserved'
            or redis.call(
                'HGET', KEYS[12], 'reservationId') ~= ARGV[1]
            or redis.call(
                'HGET', KEYS[12], 'authorityKey')
                ~= redis.call('HGET', KEYS[1], 'authorityKey')
            or redis.call(
                'HGET', KEYS[12], 'storeVersion')
                ~= redis.call('HGET', KEYS[1], 'storeVersion') then
            return {'stale', nowMs}
        end
        if redis.call('HGET', KEYS[1], 'allocationState') ~= 'pending' then
            return {'stale', nowMs}
        end
        if not leaseIsLive(KEYS[3], ARGV[4], ARGV[5]) then
            return {'stale', nowMs}
        end
        local accepted, bucket, nodeBucket = descriptorAccepts(
            KEYS[5], KEYS[6],
            ARGV[6], ARGV[7], ARGV[4], ARGV[5],
            redis.call('HGET', KEYS[1], 'objectKind'),
            redis.call('HGET', KEYS[1], 'stableType'),
            capacityKeys, 0)
        if not accepted then
            return {'stale', nowMs}
        end
        if counterValue(KEYS[2], 'storeRevision') == maxCounter then
            return {'generation-exhausted', nowMs}
        end
        local capacityDeltaValue = capacityDelta(
            redis.call('HGET', KEYS[1], 'capacityBundle'),
            redis.call('HGET', KEYS[1], 'objectKind'))
        if not capacityDeltaValue then
            return {'stale', nowMs}
        end
        local revision = incrementCounter(
            KEYS[2], 'storeRevision')
        adjustCapacity(
            capacityKeys, bucket, nodeBucket, 'pending',
            -capacityDeltaValue)
        adjustCapacity(
            capacityKeys, bucket, nodeBucket, 'active',
            capacityDeltaValue)
        redis.call('HSET', KEYS[1],
            'storeVersion', revision,
            'payload', ARGV[8],
            'allocationState', 'active')
        redis.call('HDEL', KEYS[1],
            'creationReservationId',
            'creationIntentReference',
            'creationIntentSha256',
            'creationIntentEncodedSize')
        recordHistory(
            KEYS[1], KEYS[7], KEYS[8],
            KEYS[13], KEYS[15])
        if ARGV[9] ~= '' then
            redis.call('HSET', KEYS[16],
                'state', ARGV[9],
                'terminalEnvelope', ARGV[10],
                'terminalSha256', ARGV[11],
                'expiresAtMs', ARGV[12],
                'reservationId', ARGV[1],
                'authorityKey', ARGV[13],
                'storeVersion', ARGV[14],
                'objectGeneration', ARGV[2],
                'authorityOwnerGeneration', ARGV[3],
                'targetDescriptor', ARGV[6],
                'targetLifecycle', ARGV[7],
                'targetOwner', ARGV[4],
                'targetLease', ARGV[5])
            redis.call('PEXPIREAT', KEYS[16], ARGV[12])
        end
        redis.call('DEL', KEYS[12])
        return {'committed', nowMs}
        """;
    private static final String ABORT_RESERVATION = PROLOGUE + """
        local capacityKeys = {
            KEYS[5], KEYS[8], KEYS[9], KEYS[10],
            KEYS[16], KEYS[17]}
        if ARGV[10] ~= '' then
            if tonumber(ARGV[13]) <= nowMs then
                return {'invalid-expiry', nowMs}
            end
            if redis.call('EXISTS', KEYS[15]) == 1 then
                if redis.call('HGET', KEYS[15], 'reservationId') == ARGV[1]
                    and redis.call('HGET', KEYS[15], 'state') == ARGV[10]
                    and redis.call('HGET', KEYS[15], 'terminalSha256') == ARGV[12]
                    and redis.call('HGET', KEYS[15], 'expiresAtMs') == ARGV[13]
                    and redis.call('HGET', KEYS[15], 'authorityKey') == ARGV[9]
                    and redis.call('HGET', KEYS[15], 'storeVersion') == ARGV[14] then
                    return {ARGV[10] == 'rejected'
                        and 'already-rejected' or 'already-aborted', nowMs}
                end
                return {'stale', nowMs}
            end
        end
        if redis.call('EXISTS', KEYS[1]) == 0 then
            return {ARGV[10] == ''
                and 'already-aborted' or 'stale', nowMs}
        end
        if redis.call(
                'HGET', KEYS[11], 'reservationId') ~= ARGV[1]
            or redis.call('HGET', KEYS[1], 'objectGeneration') ~= ARGV[2]
            or redis.call('HGET', KEYS[1], 'authorityOwnerGeneration') ~= ARGV[3]
            or redis.call('HGET', KEYS[1], 'ownerId') ~= ARGV[4]
            or redis.call('HGET', KEYS[1], 'ownerLeaseGeneration') ~= ARGV[5]
            or redis.call('HGET', KEYS[1], 'descriptorKey') ~= ARGV[6]
            or redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration') ~= ARGV[7] then
            return {'stale', nowMs}
        end
        if redis.call('HGET', KEYS[11], 'state') ~= 'reserved'
            or redis.call(
                'HGET', KEYS[11], 'reservationId') ~= ARGV[1]
            or redis.call(
                'HGET', KEYS[11], 'authorityKey') ~= ARGV[9]
            or redis.call(
                'HGET', KEYS[11], 'storeVersion')
                ~= redis.call('HGET', KEYS[1], 'storeVersion') then
            return {'stale', nowMs}
        end
        if redis.call('HGET', KEYS[1], 'allocationState') ~= 'pending' then
            return {'stale', nowMs}
        end
        local descriptor = redis.call(
            'HGET', KEYS[1], 'descriptorKey')
        local lifecycle = redis.call(
            'HGET', KEYS[1], 'descriptorLifecycleGeneration')
        local bucket = capacityBucket(
            descriptor, lifecycle,
            redis.call('HGET', KEYS[1], 'objectKind'),
            redis.call('HGET', KEYS[1], 'stableType'))
        local nodeBucket = nodeCapacityBucket(
            descriptor, lifecycle,
            redis.call('HGET', KEYS[1], 'objectKind'))
        if redis.call('HGET', KEYS[1], 'objectKind') == 'actor' then
            bucket = nodeBucket
        end
        local delta = capacityDelta(
            redis.call('HGET', KEYS[1], 'capacityBundle'),
            redis.call('HGET', KEYS[1], 'objectKind'))
        if not bucket or not nodeBucket or not delta then
            return {'stale', nowMs}
        end
        adjustCapacity(
            capacityKeys, bucket, nodeBucket, 'pending', -delta)
        redis.call('DEL', KEYS[1])
        redis.call('DEL', KEYS[11])
        local revision = incrementCounter(
            KEYS[3], 'storeRevision')
        recordTombstone(
            KEYS[6], KEYS[7], KEYS[12],
            KEYS[14], KEYS[13],
            revision, ARGV[9], ARGV[8])
        if ARGV[10] ~= '' then
            redis.call('HSET', KEYS[15],
                'state', ARGV[10],
                'terminalEnvelope', ARGV[11],
                'terminalSha256', ARGV[12],
                'expiresAtMs', ARGV[13],
                'reservationId', ARGV[1],
                'authorityKey', ARGV[9],
                'storeVersion', ARGV[14],
                'objectGeneration', ARGV[2],
                'authorityOwnerGeneration', ARGV[3],
                'targetDescriptor', ARGV[6],
                'targetLifecycle', ARGV[7],
                'targetOwner', ARGV[4],
                'targetLease', ARGV[5])
            redis.call('PEXPIREAT', KEYS[15], ARGV[13])
        end
        return {ARGV[10] == 'rejected'
            and 'rejected' or 'aborted', nowMs}
        """;
    private static final String RESERVE_RELOCATION_CAPACITY = PROLOGUE + """
        local capacityKeys = {
            KEYS[4], KEYS[7], KEYS[8], KEYS[9],
            KEYS[10], KEYS[11]}
        local existingState = redis.call('HGET', KEYS[5], 'state')
        if existingState then
            if redis.call('HGET', KEYS[5], 'signature') == ARGV[14] then
                return {'already-reserved', nowMs}
            end
            return {'conflict', nowMs}
        end
        if redis.call('EXISTS', KEYS[1]) == 0
            or redis.call('HGET', KEYS[1], 'storeVersion') ~= ARGV[1] then
            return {'conflict', nowMs}
        end
        if redis.call('HGET', KEYS[1], 'ownerId') ~= ARGV[2]
            or redis.call('HGET', KEYS[1], 'ownerLeaseGeneration') ~= ARGV[3]
            or redis.call('HGET', KEYS[1], 'allocationState') ~= 'active'
            or redis.call('HGET', KEYS[1], 'objectKind') ~= ARGV[6]
            or redis.call('HGET', KEYS[1], 'stableType') ~= ARGV[7]
            or redis.call('HGET', KEYS[1], 'descriptorKey') ~= ARGV[8]
            or redis.call('HGET', KEYS[1], 'descriptorLifecycleGeneration') ~= ARGV[9]
            or redis.call('HGET', KEYS[1], 'capacityBundle') ~= ARGV[10] then
            return {'conflict', nowMs}
        end
        if not leaseIsLive(KEYS[2], ARGV[4], ARGV[5]) then
            return {'target-unavailable', nowMs}
        end
        local accepted, targetBucket, targetNodeBucket =
            descriptorAccepts(
                KEYS[3], KEYS[6],
                ARGV[11], ARGV[12],
                ARGV[4], ARGV[5], ARGV[6], ARGV[7],
                capacityKeys, capacityDelta(ARGV[10], ARGV[6]))
        if not accepted then
            return {
                targetBucket == 'capacity'
                    and 'capacity-exhausted'
                    or 'target-unavailable',
                nowMs}
        end
        local sourceBucket = capacityBucket(
            redis.call('HGET', KEYS[1], 'descriptorKey'),
            redis.call(
                'HGET', KEYS[1],
                'descriptorLifecycleGeneration'),
            redis.call('HGET', KEYS[1], 'objectKind'),
            redis.call('HGET', KEYS[1], 'stableType'))
        local sourceNodeBucket = nodeCapacityBucket(
            redis.call('HGET', KEYS[1], 'descriptorKey'),
            redis.call(
                'HGET', KEYS[1],
                'descriptorLifecycleGeneration'),
            redis.call('HGET', KEYS[1], 'objectKind'))
        if redis.call('HGET', KEYS[1], 'objectKind') == 'actor' then
            sourceBucket = sourceNodeBucket
        end
        if not sourceBucket or not sourceNodeBucket
            or not adjustCapacity(
                capacityKeys, targetBucket, targetNodeBucket,
                'pending', capacityDelta(ARGV[10], ARGV[6])) then
            return {'capacity-exhausted', nowMs}
        end
        redis.call('HSET', KEYS[5],
            'state', 'reserved',
            'signature', ARGV[14],
            'authorityKey', ARGV[16],
            'expectedVersion', ARGV[1],
            'kind', ARGV[6],
            'stableType', ARGV[7],
            'sourceDescriptor', ARGV[8],
            'sourceLifecycle', ARGV[9],
            'sourceOwner', ARGV[2],
            'sourceLease', ARGV[3],
            'sourceBucket', sourceBucket,
            'sourceNodeBucket', sourceNodeBucket,
            'descriptorKey', ARGV[11],
            'targetLifecycle', ARGV[12],
            'targetOwner', ARGV[4],
            'targetLease', ARGV[5],
            'targetBucket', targetBucket,
            'targetNodeBucket', targetNodeBucket,
            'capacityBundle', ARGV[10])
        return {'reserved', nowMs}
        """;
    private static final String ABORT_RELOCATION_CAPACITY = PROLOGUE + """
        local capacityKeys = {
            KEYS[2], KEYS[3], KEYS[4], KEYS[5],
            KEYS[6], KEYS[7]}
        local state = redis.call('HGET', KEYS[1], 'state')
        if not state then return {'stale', nowMs} end
        if state == 'aborted' then
            return {'already-aborted', nowMs}
        end
        if state == 'committed' then
            return {'already-committed', nowMs}
        end
        if state ~= 'reserved' then
            return {'stale', nowMs}
        end
        local targetBucket =
            redis.call('HGET', KEYS[1], 'targetBucket')
        local targetNodeBucket =
            redis.call('HGET', KEYS[1], 'targetNodeBucket')
        local delta = capacityDelta(
            redis.call('HGET', KEYS[1], 'capacityBundle'),
            redis.call('HGET', KEYS[1], 'kind'))
        if not targetBucket or not targetNodeBucket or not delta
            or not canAdjustCapacity(
                capacityKeys, targetBucket, targetNodeBucket,
                'pending', -delta) then
            return {'stale', nowMs}
        end
        adjustCapacity(
            capacityKeys, targetBucket, targetNodeBucket,
            'pending', -delta)
        redis.call('HSET', KEYS[1], 'state', 'aborted')
        return {'aborted', nowMs}
        """;
    private static final String PREPARE_AGGREGATE = PROLOGUE + """
        local capacityKeys = {
            KEYS[3], KEYS[8], KEYS[9], KEYS[10],
            KEYS[15], KEYS[16]}
        local state = redis.call('HGET', KEYS[1], 'state')
        if state then
            if redis.call('HGET', KEYS[1], 'aggregateGeneration')
                    ~= ARGV[3] then
                return {'stale', nowMs}
            end
            if state == 'prepared'
                and redis.call('HGET', KEYS[1], 'signature')
                    == ARGV[1] then
                return {'already-prepared', nowMs}
            end
            return {'conflict', nowMs}
        end
        if not leaseIsLive(KEYS[2], ARGV[4], ARGV[5]) then
            return {'stale', nowMs}
        end
        local ok, record = pcall(cjson.decode, ARGV[6])
        if not ok or type(record) ~= 'table'
            or type(record.Participants) ~= 'table'
            or not descriptorAcceptsBundle(
                KEYS[13], KEYS[14],
                record.TargetDescriptor,
                tostring(record.TargetDescriptorLifecycleGeneration),
                ARGV[4], ARGV[5], capacityKeys,
                record.CapacityBundle) then
            return {'conflict', nowMs}
        end
        local requestedActors, requestedSpots,
            requestedKind, requestedType, requestedTypeSlots =
            capacityBundle(record.CapacityBundle)
        if not requestedActors then return {'conflict', nowMs} end
        local actors = 0
        local spots = 0
        local typeSlots = 0
        local seen = {}
        for _, participant in ipairs(record.Participants) do
            if seen[participant.AuthorityKey] then
                return {'conflict', nowMs}
            end
            seen[participant.AuthorityKey] = true
            local row = KEYS[tonumber(participant.AuthorityKeyIndex)]
            local preparedId = redis.call(
                'HGET', row, 'preparedAggregateId')
            local preparedGeneration = redis.call(
                'HGET', row, 'preparedAggregateGeneration')
            if preparedId then
                if preparedId == ARGV[2]
                    and preparedGeneration ~= ARGV[3] then
                    return {'stale', nowMs}
                end
                return {'conflict', nowMs}
            end
            if redis.call('EXISTS', row) == 0
                or redis.call('HGET', row, 'storeVersion')
                    ~= participant.ExpectedVersion
                or redis.call('HGET', row, 'allocationState')
                    ~= 'active' then
                return {'conflict', nowMs}
            end
            if participant.Transition == 'new-owner' then
                local bundle = redis.call(
                    'HGET', row, 'capacityBundle')
                local rowActors, rowSpots, rowKind, rowType,
                    rowTypeSlots = capacityBundle(bundle)
                if not rowActors
                    or (rowSpots > 0
                        and (rowKind ~= requestedKind
                            or rowType ~= requestedType))
                    or not canAdjustBundle(
                        capacityKeys,
                        redis.call('HGET', row, 'descriptorKey'),
                        redis.call(
                            'HGET', row,
                            'descriptorLifecycleGeneration'),
                        bundle, 'active', -1) then
                    return {'conflict', nowMs}
                end
                local accepted = descriptorAccepts(
                    KEYS[13], KEYS[14],
                    record.TargetDescriptor,
                    tostring(record.TargetDescriptorLifecycleGeneration),
                    ARGV[4], ARGV[5],
                    redis.call('HGET', row, 'objectKind'),
                    redis.call('HGET', row, 'stableType'),
                    capacityKeys, 0)
                if not accepted then return {'conflict', nowMs} end
                actors = actors + rowActors
                spots = spots + rowSpots
                typeSlots = typeSlots + rowTypeSlots
            elseif requestedActors == 0 and requestedSpots == 0
                and participant.MembershipMutation ~= '' then
                return {'conflict', nowMs}
            end
        end
        if actors ~= requestedActors or spots ~= requestedSpots
            or typeSlots ~= requestedTypeSlots then
            return {'conflict', nowMs}
        end
        adjustBundle(
            capacityKeys, record.TargetDescriptor,
            tostring(record.TargetDescriptorLifecycleGeneration),
            record.CapacityBundle, 'pending', 1)
        redis.call('HSET', KEYS[1],
            'state', 'prepared',
            'signature', ARGV[1],
            'aggregateId', ARGV[2],
            'aggregateGeneration', ARGV[3],
            'targetOwner', ARGV[4],
            'targetLease', ARGV[5],
            'targetDescriptor', record.TargetDescriptor,
            'targetLifecycle',
                tostring(record.TargetDescriptorLifecycleGeneration),
            'capacityBundle', record.CapacityBundle,
            'record', ARGV[6])
        for _, participant in ipairs(record.Participants) do
            redis.call(
                'HSET',
                KEYS[tonumber(participant.AuthorityKeyIndex)],
                'preparedAggregateId', ARGV[2],
                'preparedAggregateGeneration', ARGV[3])
        end
        return {'prepared', nowMs}
        """;
    private static final String COMMIT_AGGREGATE = PROLOGUE + """
        local capacityKeys = {
            KEYS[3], KEYS[8], KEYS[9], KEYS[10],
            KEYS[15], KEYS[16]}
        if redis.call('HGET', KEYS[1], 'aggregateGeneration')
                ~= ARGV[1] then
            return {'aggregate-stale', nowMs}
        end
        local state = redis.call('HGET', KEYS[1], 'state')
        if state == 'committed' then
            return {'already-committed', nowMs}
        end
        if state ~= 'prepared' then return {'aggregate-stale', nowMs} end
        local targetOwner = redis.call(
            'HGET', KEYS[1], 'targetOwner')
        local targetLease = redis.call(
            'HGET', KEYS[1], 'targetLease')
        if not leaseIsLive(
            KEYS[2], targetOwner, targetLease) then
            return {'target-owner-stale', nowMs}
        end
        local ok, record = pcall(
            cjson.decode, redis.call('HGET', KEYS[1], 'record'))
        if not ok or type(record) ~= 'table'
            or type(record.Participants) ~= 'table' then
            return {'aggregate-stale', nowMs}
        end
        local targetDescriptor = redis.call(
            'HGET', KEYS[1], 'targetDescriptor')
        local targetLifecycle = redis.call(
            'HGET', KEYS[1], 'targetLifecycle')
        local bundle = redis.call(
            'HGET', KEYS[1], 'capacityBundle')
        local ownerIncrements = 0
        for _, participant in ipairs(record.Participants) do
            local row = KEYS[tonumber(participant.AuthorityKeyIndex)]
            if redis.call('EXISTS', row) == 0
                or redis.call('HGET', row, 'storeVersion')
                    ~= participant.ExpectedVersion
                or redis.call('HGET', row, 'allocationState')
                    ~= 'active'
                or redis.call('HGET', row, 'preparedAggregateId')
                    ~= redis.call('HGET', KEYS[1], 'aggregateId')
                or redis.call(
                    'HGET', row, 'preparedAggregateGeneration')
                    ~= ARGV[1] then
                return {'participant-stale', nowMs}
            end
            if participant.Transition == 'new-owner' then
                ownerIncrements = ownerIncrements + 1
                if not descriptorAccepts(
                        KEYS[13], KEYS[14],
                        targetDescriptor, targetLifecycle,
                        targetOwner, targetLease,
                        redis.call('HGET', row, 'objectKind'),
                        redis.call('HGET', row, 'stableType'),
                        capacityKeys, 0)
                    or not canAdjustBundle(
                        capacityKeys,
                        redis.call('HGET', row, 'descriptorKey'),
                        redis.call(
                            'HGET', row,
                            'descriptorLifecycleGeneration'),
                        redis.call('HGET', row, 'capacityBundle'),
                        'active', -1) then
                    return {'participant-stale', nowMs}
                end
            end
        end
        if not canAdjustBundle(
                capacityKeys, targetDescriptor, targetLifecycle,
                bundle, 'pending', -1)
            or not canIncrementBy(
                counterValue(KEYS[4], 'storeRevision'),
                #record.Participants)
            or not canIncrementBy(
                counterValue(
                    KEYS[5], 'authorityOwnerGeneration'),
                ownerIncrements) then
            return {'generation-exhausted', nowMs}
        end
        for _, participant in ipairs(record.Participants) do
            local row = KEYS[tonumber(participant.AuthorityKeyIndex)]
            local revision = incrementCounter(
                KEYS[4], 'storeRevision')
            local ownerGeneration = redis.call(
                'HGET', row, 'authorityOwnerGeneration')
            if participant.Transition == 'new-owner' then
                adjustBundle(
                    capacityKeys,
                    redis.call('HGET', row, 'descriptorKey'),
                    redis.call(
                        'HGET', row,
                        'descriptorLifecycleGeneration'),
                    redis.call('HGET', row, 'capacityBundle'),
                    'active', -1)
                ownerGeneration = incrementCounter(
                    KEYS[5], 'authorityOwnerGeneration')
                redis.call('HSET', row,
                    'ownerId', targetOwner,
                    'ownerLeaseGeneration', targetLease,
                    'descriptorKey', targetDescriptor,
                    'descriptorLifecycleGeneration',
                        targetLifecycle)
            end
            redis.call('HSET', row,
                'storeVersion', revision,
                'payload', participant.Payload,
                'authorityOwnerGeneration', ownerGeneration)
            redis.call('HDEL', row,
                'preparedAggregateId',
                'preparedAggregateGeneration')
            recordHistory(
                row,
                KEYS[tonumber(participant.HistoryKeyIndex)],
                KEYS[tonumber(
                    participant.HistoryRevisionsKeyIndex)],
                KEYS[11], KEYS[12])
            redis.call('HSET', KEYS[7],
                participant.AuthorityKey,
                participant.MembershipMutation)
            recordMembership(
                KEYS[tonumber(
                    participant.MembershipHistoryKeyIndex)],
                KEYS[tonumber(
                    participant.MembershipHistoryRevisionsKeyIndex)],
                revision,
                participant.MembershipMutation)
            redis.call('ZADD', KEYS[6], 0,
                participant.EncodedAuthorityKey)
        end
        adjustBundle(
            capacityKeys, targetDescriptor, targetLifecycle,
            bundle, 'pending', -1)
        adjustBundle(
            capacityKeys, targetDescriptor, targetLifecycle,
            bundle, 'active', 1)
        redis.call('HSET', KEYS[1], 'state', 'committed')
        return {'committed', nowMs}
        """;
    private static final String ABORT_AGGREGATE = PROLOGUE + """
        local capacityKeys = {
            KEYS[3], KEYS[8], KEYS[9], KEYS[10],
            KEYS[15], KEYS[16]}
        if redis.call('HGET', KEYS[1], 'aggregateGeneration')
                ~= ARGV[1] then
            return {'stale', nowMs}
        end
        local state = redis.call('HGET', KEYS[1], 'state')
        if state == 'aborted' then
            return {'already-aborted', nowMs}
        end
        if state ~= 'prepared' then return {'stale', nowMs} end
        local ok, record = pcall(
            cjson.decode, redis.call('HGET', KEYS[1], 'record'))
        if not ok or type(record) ~= 'table'
            or type(record.Participants) ~= 'table' then
            return {'stale', nowMs}
        end
        local targetDescriptor = redis.call(
            'HGET', KEYS[1], 'targetDescriptor')
        local targetLifecycle = redis.call(
            'HGET', KEYS[1], 'targetLifecycle')
        local bundle = redis.call(
            'HGET', KEYS[1], 'capacityBundle')
        if not canAdjustBundle(
                capacityKeys, targetDescriptor, targetLifecycle,
                bundle, 'pending', -1) then
            return {'stale', nowMs}
        end
        adjustBundle(
            capacityKeys, targetDescriptor, targetLifecycle,
            bundle, 'pending', -1)
        for _, participant in ipairs(record.Participants) do
            local row = KEYS[tonumber(participant.AuthorityKeyIndex)]
            if redis.call('HGET', row, 'preparedAggregateId')
                    == redis.call('HGET', KEYS[1], 'aggregateId') then
                redis.call('HDEL', row,
                    'preparedAggregateId',
                    'preparedAggregateGeneration')
            end
        end
        redis.call('HSET', KEYS[1], 'state', 'aborted')
        return {'aborted', nowMs}
        """;
    private static final String START_SCAN = PROLOGUE + """
        local expired = redis.call(
            'ZRANGEBYSCORE', KEYS[3], '-inf', nowMs,
            'LIMIT', 0, 32)
        for _, member in ipairs(expired) do
            redis.call('ZREM', KEYS[3], member)
            redis.call('ZREM', KEYS[4], member)
        end
        local watermark = counterValue(KEYS[1], 'storeRevision')
        local oldest = redis.call('ZRANGE', KEYS[4], 0, 0)[1]
        local gcWatermark = oldest
            and string.sub(oldest, 1, 16)
            or decimalToHex16(watermark)
        local retired = redis.call(
            'ZRANGEBYLEX', KEYS[6], '-',
            '[' .. gcWatermark, 'LIMIT', 0, 32)
        for _, member in ipairs(retired) do
            redis.call('ZREM', KEYS[5],
                string.sub(member, 18))
            redis.call('ZREM', KEYS[6], member)
        end
        local expiresAt = nowMs + tonumber(ARGV[2])
        local scanMember =
            decimalToHex16(watermark) .. ':' .. ARGV[1]
        redis.call('HSET', KEYS[2],
            'scanId', ARGV[1],
            'watermark', watermark,
            'offset', '0',
            'expiresAt', expiresAt)
        redis.call('PEXPIREAT', KEYS[2], expiresAt)
        redis.call('ZADD', KEYS[3], expiresAt, scanMember)
        redis.call('ZADD', KEYS[4], 0, scanMember)
        return {watermark, nowMs}
        """;
    private static final String LIST = PROLOGUE + """
        if redis.call('HGET', KEYS[3], 'scanId') ~= ARGV[1]
            or redis.call('HGET', KEYS[3], 'watermark') ~= ARGV[2]
            or redis.call('HGET', KEYS[3], 'offset') ~= ARGV[4]
            or tonumber(redis.call(
                'HGET', KEYS[3], 'expiresAt') or '0') <= nowMs then
            return {'expired', nowMs}
        end
        local out = {}
        local encodedBytes = 0
        local processed = 0
        for index = 6, #KEYS, 3 do
            local history = KEYS[index + 1]
            local revisions = KEYS[index + 2]
            local found = redis.call(
                'ZREVRANGEBYLEX', revisions,
                '[' .. ARGV[3], '-',
                'LIMIT', 0, 1)
            local member = found[1]
            local function field(name)
                return redis.call(
                    'HGET', history, member .. ':' .. name)
            end
            local key = member and field('authorityKey') or nil
            if member
                and field('deleted') ~= '1'
                and key
                and string.sub(key, 1, string.len(ARGV[6]))
                    == ARGV[6] then
                local values = {}
                local rowBytes = string.len(key)
                for _, name in ipairs(historyFields) do
                    if name ~= 'authorityKey' then
                        local value = field(name)
                        values[#values + 1] = value
                        rowBytes = rowBytes + string.len(value or '')
                    end
                end
                if #out > 0
                    and encodedBytes + rowBytes > 4194304 then
                    break
                end
                out[#out + 1] = key
                for _, value in ipairs(values) do
                    out[#out + 1] = value
                end
                encodedBytes = encodedBytes + rowBytes
            end
            processed = processed + 1
        end
        local offset = tonumber(ARGV[4])
        local nextOffset = offset + processed
        local total = redis.call('ZCARD', KEYS[1])
        if nextOffset < total then
            redis.call('HSET', KEYS[3], 'offset', nextOffset)
        else
            redis.call('DEL', KEYS[3])
            local scanMember =
                ARGV[3] .. ':' .. ARGV[1]
            redis.call('ZREM', KEYS[4], scanMember)
            redis.call('ZREM', KEYS[5], scanMember)
            nextOffset = -1
        end
        return {'page', nowMs, ARGV[1], ARGV[2],
            nextOffset, out}
        """;

    private final ZLinkRedisLocationConnection connection;
    private final ZLinkRedisLocationKeys keys;

    ZLinkRedisAuthorityClient(
        ZLinkRedisLocationConnection connection,
        ZLinkRedisLocationKeys keys) {
        this.connection = connection;
        this.keys = keys;
    }

    CompletionStage<ZLinkAuthorityReadResult> read(
        String key,
        ZLinkStoreCancellation cancellation) {
        requireKey(key);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                READ,
                ScriptOutputType.MULTI,
                new String[] {keys.authorityRowKey(key)}))
            .thenApply(this::readResult);
    }

    CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
        String key,
        ZLinkAuthorityExpectation expectation,
        ZLinkAuthorityMutation mutation,
        ZLinkStoreCancellation cancellation) {
        requireKey(key);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        String expectationKind = "found";
        String version =
            ((ZLinkAuthorityExpectFound) expectation).storeVersion();
        boolean deleting = mutation instanceof ZLinkAuthorityDelete;
        boolean restoring = mutation instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRestore;
        byte[] mutationPayload = deleting
            ? new byte[0]
            : restoring
                ? ((systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRestore)
                    mutation).payload()
                : ((ZLinkAuthorityPut) mutation).payload();
        String payload = encode(mutationPayload);
        ZLinkAuthorityPut put = deleting || restoring
            ? null
            : (ZLinkAuthorityPut) mutation;
        String transition = deleting
            ? "preserve"
            : switch (put.generationTransition()) {
                case PRESERVE -> "preserve";
                case NEW_OWNER -> "new-owner";
            };
        ZLinkLocationOwnerToken targetOwner = restoring
            ? ((systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRestore)
                mutation).expectedOwner()
            : put == null ? null : put.targetOwner().orElse(null);
        String capacityFence = put == null
            ? ""
            : put.relocationCapacityFence()
                .map(ZLinkRelocationCapacityFence::value)
                .orElse("");
        String leaseOwner = targetOwner == null
            ? ""
            : targetOwner.ownerId();
        CompletionStage<String> leaseOwnerStage =
            !leaseOwner.isEmpty()
                ? CompletableFuture.completedFuture(leaseOwner)
                : read(key, cancellation).thenApply(current ->
                    current instanceof ZLinkAuthoritySnapshot snapshot
                        ? snapshot.ownerId()
                        : "");
        CompletionStage<Optional<ZLinkMeshNodeDescriptorKey>>
            targetDescriptorStage =
            "new-owner".equals(transition)
                ? connection.commands().thenCompose(redis ->
                    redis.hget(
                        keys.relocationCapacityKey(capacityFence),
                        "descriptorKey"))
                    .thenApply(value -> Optional.of(
                        descriptor(value)))
                : CompletableFuture.completedFuture(
                    Optional.empty());
        return leaseOwnerStage.thenCombine(
            targetDescriptorStage,
            CasInputs::new)
            .thenCompose(inputs ->
            connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                CAS,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.authorityRowKey(key),
                    keys.authorityIndexKey(),
                    keys.authorityRevisionKey(),
                    keys.leaseKey(inputs.leaseOwner),
                    keys.authorityObjectGenerationKey(),
                    keys.authorityOwnerGenerationKey(),
                    keys.relocationCapacityKey(
                        capacityFence.isEmpty()
                            ? "unused"
                            : capacityFence),
                    keys.capacityTypeActiveKey(),
                    inputs.targetDescriptor
                        .map(keys::meshNodeDescriptorRowKey)
                        .orElse(keys.schemaKey()),
                    inputs.targetDescriptor
                        .map(keys::meshNodeDescriptorMetadataKey)
                        .orElse(keys.schemaKey()),
                    keys.authorityHistoryKey(key),
                    keys.authorityHistoryRevisionsKey(key),
                    keys.capacityTypePendingKey(),
                    keys.capacityNodeActiveKey(),
                    keys.capacityNodePendingKey(),
                    keys.scansWatermarkKey(),
                    keys.authorityIndexGcKey(),
                    keys.scansExpiryKey(),
                    keys.capacitySpotActiveKey(),
                    keys.capacitySpotReservedKey()
                },
                expectationKind,
                version,
                deleting ? "delete" : restoring ? "restore" : "put",
                transition,
                payload,
                keys.encodedAuthorityKey(key),
                targetOwner == null ? "" : targetOwner.ownerId(),
                targetOwner == null
                    ? ""
                    : Long.toString(targetOwner.leaseGeneration()),
                capacityFence,
                key)))
            .thenCompose(raw -> {
                String status = string(raw.getFirst());
                if ("conflict".equals(status)) {
                    return read(key, cancellation)
                        .thenApply(ZLinkAuthorityConflict::new);
                }
                Instant now = time(raw, 1);
                if ("deleted".equals(status)) {
                    return CompletableFuture.completedFuture(
                        new ZLinkAuthorityDeleted(string(raw.get(2)), now));
                }
                if ("generation-exhausted".equals(status)) {
                    return CompletableFuture.completedFuture(
                        new ZLinkAuthorityGenerationExhausted());
                }
                if (!"stored".equals(status)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "unsupported authority transition: " + status));
                }
                return CompletableFuture.completedFuture(new ZLinkAuthorityStored(
                    string(raw.get(2)),
                    Base64.getDecoder().decode(payload),
                    number(raw.get(3)),
                    number(raw.get(4)),
                    string(raw.get(5)),
                    number(raw.get(6)),
                    allocation(raw, 7),
                    now));
            });
    }

    CompletionStage<ZLinkAuthorityScanResult> list(
        String prefix,
        Optional<ZLinkAuthorityScanCursor> cursor,
        int limit,
        ZLinkStoreCancellation cancellation) {
        if (prefix == null || limit < 1 || limit > 1000) {
            throw new IllegalArgumentException("authority scan limit must be 1..1000");
        }
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        CompletionStage<Cursor> scan = cursor.isPresent()
            ? CompletableFuture.completedFuture(
                decodeCursor(cursor.get()))
            : startScan();
        return scan.thenCompose(decoded ->
            connection.commands().thenCompose(redis ->
                    redis.zrange(
                            keys.authorityIndexKey(),
                            decoded.offset,
                            decoded.offset + limit - 1L)
                        .thenCompose(encodedKeys -> {
                            String[] scriptKeys =
                                new String[
                                    encodedKeys.size() * 3 + 5];
                            scriptKeys[0] = keys.authorityIndexKey();
                            scriptKeys[1] = keys.counterKey();
                            scriptKeys[2] = keys.scanKey(
                                decoded.scanId);
                            scriptKeys[3] = keys.scansExpiryKey();
                            scriptKeys[4] = keys.scansWatermarkKey();
                            for (int index = 0;
                                index < encodedKeys.size();
                                index++) {
                                String authorityKey =
                                    keys.decodeAuthorityKey(
                                        encodedKeys.get(index));
                                int keyOffset = index * 3 + 5;
                                scriptKeys[keyOffset] =
                                    keys.authorityRowKey(authorityKey);
                                scriptKeys[keyOffset + 1] =
                                    keys.authorityHistoryKey(
                                        authorityKey);
                                scriptKeys[keyOffset + 2] =
                                    keys.authorityHistoryRevisionsKey(
                                        authorityKey);
                            }
                            return redis.<List<Object>>eval(
                                LIST,
                                ScriptOutputType.MULTI,
                                scriptKeys,
                                uuidHex(decoded.scanId),
                                decoded.watermark,
                                fixedHex(decoded.watermark),
                                Integer.toString(decoded.offset),
                                Integer.toString(limit),
                                prefix);
                        })))
            .thenApply(raw -> toPage(raw));
    }

    private CompletionStage<Cursor> startScan() {
        java.util.UUID scanId = java.util.UUID.randomUUID();
        String scanHex = uuidHex(scanId);
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                START_SCAN,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.counterKey(),
                    keys.scanKey(scanId),
                    keys.scansExpiryKey(),
                    keys.scansWatermarkKey(),
                    keys.authorityIndexKey(),
                    keys.authorityIndexGcKey()
                },
                scanHex,
                "300000"))
            .thenApply(raw -> new Cursor(
                scanId,
                string(raw.getFirst()),
                0));
    }

    CompletionStage<ZLinkObjectReserveResult> reserve(
        ZLinkObjectReservationRequest request,
        ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        String key = request.authorityKey();
        String encodedKey = keys.encodedAuthorityKey(key);
        String reservationId = uuidHex(
            java.util.UUID.randomUUID());
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                RESERVE,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.authorityRowKey(key),
                    keys.authorityIndexKey(),
                    keys.authorityRevisionKey(),
                    keys.authorityObjectGenerationKey(),
                    keys.authorityOwnerGenerationKey(),
                    keys.leaseKey(request.targetOwner().ownerId()),
                    keys.capacityTypeActiveKey(),
                    keys.meshNodeDescriptorRowKey(
                        request.targetDescriptor()),
                    keys.meshNodeDescriptorMetadataKey(
                        request.targetDescriptor()),
                    keys.authorityHistoryKey(key),
                    keys.authorityHistoryRevisionsKey(key),
                    keys.capacityTypePendingKey(),
                    keys.capacityNodeActiveKey(),
                    keys.capacityNodePendingKey(),
                    keys.creationKey(reservationId),
                    keys.scansWatermarkKey(),
                    keys.scansExpiryKey(),
                    request.objectKind()
                        == systems.zlink.framework.locations.ZLinkPlacementObjectKind.ACTOR
                        ? keys.schemaKey()
                        : keys.entrySpotIdentityClaimKeyFromAuthority(key),
                    keys.capacitySpotActiveKey(),
                    keys.capacitySpotReservedKey()
                },
                encodedKey,
                request.stableType(),
                request.targetOwner().ownerId(),
                Long.toString(request.targetOwner().leaseGeneration()),
                request.creationIntentReference(),
                encode(request.creationIntentHash()),
                key,
                encode(request.creatingPayload()),
                descriptorKey(request.targetDescriptor()),
                Long.toString(
                    request.targetDescriptorLifecycleGeneration()),
                objectKindToken(request.objectKind()),
                encodeCapacityBundle(request.capacityBundle()),
                reservationId,
                Integer.toString(request.creationIntentEncodedSize())))
            .thenApply(raw -> reserveResult(request, raw));
    }

    CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkStoreCancellation cancellation) {
        return commit(reservation, readyPayload, null, cancellation);
    }

    CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        if (terminal != null) {
            validateTerminal(
                reservation,
                terminal,
                ZLinkCreationTerminalState.CREATED);
        }
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                COMMIT_RESERVATION,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.authorityRowKey(reservation.authorityKey()),
                    keys.authorityRevisionKey(),
                    keys.leaseKey(
                        reservation.targetOwner().ownerId()),
                    keys.capacityTypeActiveKey(),
                    keys.meshNodeDescriptorRowKey(
                        reservation.targetDescriptor()),
                    keys.meshNodeDescriptorMetadataKey(
                        reservation.targetDescriptor()),
                    keys.authorityHistoryKey(
                        reservation.authorityKey()),
                    keys.authorityHistoryRevisionsKey(
                        reservation.authorityKey()),
                    keys.capacityTypePendingKey(),
                    keys.capacityNodeActiveKey(),
                    keys.capacityNodePendingKey(),
                    keys.creationKey(
                        reservation.reservationVersion()),
                    keys.scansWatermarkKey(),
                    keys.authorityIndexGcKey(),
                    keys.scansExpiryKey(),
                    terminal == null
                        ? keys.creationKey(reservation.reservationVersion())
                        : keys.creationTerminalKey(terminal.operation()),
                    keys.capacitySpotActiveKey(),
                    keys.capacitySpotReservedKey()
                },
                reservation.reservationVersion(),
                Long.toString(reservation.objectGeneration()),
                Long.toString(reservation.authorityOwnerGeneration()),
                reservation.targetOwner().ownerId(),
                Long.toString(reservation.targetOwner().leaseGeneration()),
                descriptorKey(reservation.targetDescriptor()),
                Long.toString(
                    reservation.targetDescriptorLifecycleGeneration()),
                encode(readyPayload),
                terminal == null ? "" : terminalState(terminal.state()),
                terminal == null ? "" : encode(terminal.terminalEnvelope()),
                terminal == null ? "" : encode(terminal.terminalSha256()),
                terminal == null ? "" : Long.toString(
                    terminal.expiresAt().toEpochMilli()),
                reservation.authorityKey(),
                reservation.storeVersion()))
            .thenApply(raw -> switch (string(raw.getFirst())) {
                case "committed" -> ZLinkObjectCommitResult.COMMITTED;
                case "already-committed" -> ZLinkObjectCommitResult.ALREADY_COMMITTED;
                case "generation-exhausted" ->
                    ZLinkObjectCommitResult.GENERATION_EXHAUSTED;
                case "invalid-expiry" -> throw new IllegalArgumentException(
                    "terminal expiresAt must be later than provider store time");
                default -> ZLinkObjectCommitResult.STALE;
            });
    }

    CompletionStage<ZLinkObjectRejectResult> reject(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        validateTerminal(
            reservation,
            terminal,
            ZLinkCreationTerminalState.REJECTED);
        return abortOrReject(
            reservation,
            terminal,
            cancellation).thenApply(status -> switch (status) {
                case "rejected" -> ZLinkObjectRejectResult.REJECTED;
                case "already-rejected" ->
                    ZLinkObjectRejectResult.ALREADY_REJECTED;
                case "generation-exhausted" ->
                    ZLinkObjectRejectResult.GENERATION_EXHAUSTED;
                case "invalid-expiry" -> throw new IllegalArgumentException(
                    "terminal expiresAt must be later than provider store time");
                default -> ZLinkObjectRejectResult.STALE;
            });
    }

    CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkStoreCancellation cancellation) {
        return abortOrReject(reservation, null, cancellation)
            .thenApply(ZLinkRedisAuthorityClient::abortResult);
    }

    CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        validateTerminal(
            reservation,
            terminal,
            ZLinkCreationTerminalState.FAILED);
        return abortOrReject(reservation, terminal, cancellation)
            .thenApply(status -> {
                if ("invalid-expiry".equals(status)) {
                    throw new IllegalArgumentException(
                        "terminal expiresAt must be later than provider store time");
                }
                return abortResult(status);
            });
    }

    private CompletionStage<String> abortOrReject(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ABORT_RESERVATION,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.authorityRowKey(reservation.authorityKey()),
                    keys.authorityIndexKey(),
                    keys.authorityRevisionKey(),
                    keys.leaseKey(
                        reservation.targetOwner().ownerId()),
                    keys.capacityTypeActiveKey()
                    ,
                    keys.authorityHistoryKey(
                        reservation.authorityKey()),
                    keys.authorityHistoryRevisionsKey(
                        reservation.authorityKey()),
                    keys.capacityTypePendingKey(),
                    keys.capacityNodeActiveKey(),
                    keys.capacityNodePendingKey(),
                    keys.creationKey(
                        reservation.reservationVersion()),
                    keys.scansWatermarkKey(),
                    keys.authorityIndexGcKey(),
                    keys.scansExpiryKey(),
                    terminal == null
                        ? keys.creationKey(reservation.reservationVersion())
                        : keys.creationTerminalKey(terminal.operation()),
                    keys.capacitySpotActiveKey(),
                    keys.capacitySpotReservedKey()
                },
                reservation.reservationVersion(),
                Long.toString(reservation.objectGeneration()),
                Long.toString(reservation.authorityOwnerGeneration()),
                reservation.targetOwner().ownerId(),
                Long.toString(reservation.targetOwner().leaseGeneration()),
                descriptorKey(reservation.targetDescriptor()),
                Long.toString(
                    reservation.targetDescriptorLifecycleGeneration()),
                keys.encodedAuthorityKey(reservation.authorityKey()),
                reservation.authorityKey(),
                terminal == null ? "" : terminalState(terminal.state()),
                terminal == null ? "" : encode(terminal.terminalEnvelope()),
                terminal == null ? "" : encode(terminal.terminalSha256()),
                terminal == null ? "" : Long.toString(
                    terminal.expiresAt().toEpochMilli()),
                reservation.storeVersion()))
            .thenApply(raw -> string(raw.getFirst()));
    }

    CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
        ZLinkCreationOperationIdentity operation,
        ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.hgetall(
                keys.creationTerminalKey(operation)))
            .thenApply(fields -> decodeCreationTerminal(operation, fields));
    }

    CompletionStage<ZLinkRelocationCapacityReserveResult>
        reserveRelocationCapacity(
            ZLinkRelocationCapacityReservationRequest request,
            ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        String fence = request.reservationId().toString();
        String signature = relocationSignature(request);
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                RESERVE_RELOCATION_CAPACITY,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.authorityRowKey(request.authorityKey()),
                    keys.leaseKey(
                        request.targetOwner().ownerId()),
                    keys.meshNodeDescriptorRowKey(
                        request.targetDescriptor()),
                    keys.capacityTypeActiveKey(),
                    keys.relocationCapacityKey(fence)
                    ,
                    keys.meshNodeDescriptorMetadataKey(
                        request.targetDescriptor()),
                    keys.capacityTypePendingKey(),
                    keys.capacityNodeActiveKey(),
                    keys.capacityNodePendingKey(),
                    keys.capacitySpotActiveKey(),
                    keys.capacitySpotReservedKey()
                },
                request.expectedStoreVersion(),
                request.sourceOwner().ownerId(),
                Long.toString(
                    request.sourceOwner().leaseGeneration()),
                request.targetOwner().ownerId(),
                Long.toString(
                    request.targetOwner().leaseGeneration()),
                objectKindToken(request.objectKind()),
                request.stableType(),
                descriptorKey(request.sourceDescriptor()),
                Long.toString(
                    request.sourceDescriptorLifecycleGeneration()),
                encodeCapacityBundle(request.capacityBundle()),
                descriptorKey(request.targetDescriptor()),
                Long.toString(
                    request.targetDescriptorLifecycleGeneration()),
                "",
                signature,
                keys.meshNodeDescriptorRowKey(
                    request.targetDescriptor()),
                request.authorityKey()))
            .thenCompose(raw -> {
                String status = string(raw.getFirst());
                if ("conflict".equals(status)) {
                    return read(request.authorityKey(), cancellation)
                        .thenApply(
                            ZLinkRelocationCapacityConflict::new);
                }
                ZLinkRelocationCapacityFence capacityFence =
                    new ZLinkRelocationCapacityFence(fence);
                return CompletableFuture.completedFuture(
                    switch (status) {
                        case "reserved" ->
                            new ZLinkRelocationCapacityReserved(
                                capacityFence);
                        case "already-reserved" ->
                            new ZLinkRelocationCapacityAlreadyReserved(
                                capacityFence);
                        case "capacity-exhausted" ->
                            new ZLinkRelocationCapacityExhausted();
                        default ->
                            new ZLinkRelocationCapacityTargetUnavailable();
                    });
            });
    }

    CompletionStage<ZLinkRelocationCapacityAbortResult>
        abortRelocationCapacity(
            ZLinkRelocationCapacityFence fence,
            ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                ABORT_RELOCATION_CAPACITY,
                ScriptOutputType.MULTI,
                new String[] {
                    keys.relocationCapacityKey(fence.value()),
                    keys.capacityTypeActiveKey(),
                    keys.capacityTypePendingKey(),
                    keys.capacityNodeActiveKey(),
                    keys.capacityNodePendingKey(),
                    keys.capacitySpotActiveKey(),
                    keys.capacitySpotReservedKey()
                }))
            .thenApply(raw -> switch (string(raw.getFirst())) {
                case "aborted" ->
                    ZLinkRelocationCapacityAbortResult.ABORTED;
                case "already-aborted" ->
                    ZLinkRelocationCapacityAbortResult.ALREADY_ABORTED;
                case "already-committed" ->
                    ZLinkRelocationCapacityAbortResult.ALREADY_COMMITTED;
                default -> ZLinkRelocationCapacityAbortResult.STALE;
            });
    }

    CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
        ZLinkAggregatePrepareRequest request,
        ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        AggregateEncoding aggregate = encodeAggregate(request);
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                PREPARE_AGGREGATE,
                ScriptOutputType.MULTI,
                aggregate.keys.toArray(String[]::new),
                aggregate.signature,
                request.aggregateId().toString(),
                Long.toString(request.aggregateGeneration()),
                request.targetOwner().ownerId(),
                Long.toString(
                    request.targetOwner().leaseGeneration()),
                aggregate.json))
            .thenApply(raw -> {
                trace("aggregate-prepare id="
                    + request.aggregateId()
                    + " generation=" + request.aggregateGeneration()
                    + " status=" + string(raw.getFirst()));
                return switch (string(raw.getFirst())) {
                case "prepared" ->
                    new ZLinkAggregatePrepared(
                        new ZLinkAggregateFence(
                            request.aggregateId(),
                            request.aggregateGeneration()));
                case "already-prepared" ->
                    new ZLinkAggregateAlreadyPrepared(
                        new ZLinkAggregateFence(
                            request.aggregateId(),
                            request.aggregateGeneration()));
                case "stale" -> new ZLinkAggregateStale();
                case "generation-exhausted" ->
                    new ZLinkAggregateGenerationExhausted();
                default -> new ZLinkAggregateConflict();
                };
            });
    }

    CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        return commitAggregate(
            fence,
            cancellation,
            0,
            Instant.now().plus(AGGREGATE_COMMIT_RETRY_WINDOW));
    }

    private CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation,
        int retryAttempt,
        Instant retryDeadline) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        String aggregateKey = keys.authorityAggregateKey(
            fence.aggregateId(),
            fence.aggregateGeneration());
        return connection.commands()
            .thenCompose(redis -> redis.hgetall(aggregateKey))
            .thenCompose(fields -> {
                String[] operationKeys =
                    aggregateOperationKeys(aggregateKey, fields);
                return connection.commands()
                .thenCompose(redis -> redis.<List<Object>>eval(
                    COMMIT_AGGREGATE,
                    ScriptOutputType.MULTI,
                    operationKeys,
                    Long.toString(fence.aggregateGeneration())));
            })
            .thenCompose(raw -> {
                String status = string(raw.getFirst());
                trace("aggregate-commit id="
                    + fence.aggregateId()
                    + " generation=" + fence.aggregateGeneration()
                    + " attempt=" + retryAttempt
                    + " status=" + status);
                if ("target-owner-stale".equals(status)
                    && retryAttempt < AGGREGATE_COMMIT_RETRY_LIMIT
                    && Instant.now().isBefore(retryDeadline)
                    && !cancelled(cancellation)) {
                    return delayAggregateCommitRetry(
                            retryAttempt,
                            retryDeadline,
                            cancellation)
                        .thenCompose(ignored -> commitAggregate(
                            fence,
                            cancellation,
                            retryAttempt + 1,
                            retryDeadline));
                }
                return CompletableFuture.completedFuture(
                    switch (status) {
                        case "committed" ->
                            ZLinkAggregateCommitResult.COMMITTED;
                        case "already-committed" ->
                            ZLinkAggregateCommitResult.ALREADY_COMMITTED;
                        case "generation-exhausted" ->
                            ZLinkAggregateCommitResult.GENERATION_EXHAUSTED;
                        default -> ZLinkAggregateCommitResult.STALE;
                    });
            });
    }

    private static CompletionStage<Void> delayAggregateCommitRetry(
        int retryAttempt,
        Instant retryDeadline,
        ZLinkStoreCancellation cancellation) {
        long remainingMillis = Duration.between(
                Instant.now(), retryDeadline).toMillis();
        if (remainingMillis <= 0 || cancelled(cancellation)) {
            return CompletableFuture.completedFuture(null);
        }
        long baseMillis = Math.min(
            100L,
            2L << Math.min(retryAttempt, 5));
        long jitterMillis =
            java.util.concurrent.ThreadLocalRandom.current()
                .nextLong(baseMillis + 1L);
        long delayMillis = Math.min(
            remainingMillis,
            baseMillis + jitterMillis);
        return CompletableFuture.runAsync(
            () -> {},
            CompletableFuture.delayedExecutor(
                delayMillis,
                TimeUnit.MILLISECONDS));
    }

    private static void trace(String message) {
        if (TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] redis-authority "
                + message);
        }
    }

    CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        String aggregateKey = keys.authorityAggregateKey(
            fence.aggregateId(),
            fence.aggregateGeneration());
        return connection.commands()
            .thenCompose(redis -> redis.hgetall(aggregateKey))
            .thenCompose(fields -> connection.commands()
                .thenCompose(redis -> redis.<List<Object>>eval(
                    ABORT_AGGREGATE,
                    ScriptOutputType.MULTI,
                    aggregateOperationKeys(aggregateKey, fields),
                    Long.toString(fence.aggregateGeneration()))))
            .thenApply(raw -> switch (string(raw.getFirst())) {
                case "aborted" ->
                    ZLinkAggregateAbortResult.ABORTED;
                case "already-aborted" ->
                    ZLinkAggregateAbortResult.ALREADY_ABORTED;
                default -> ZLinkAggregateAbortResult.STALE;
            });
    }

    CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
        projectMeshNodeCapacity(
            ZLinkLocationPage<ZLinkMeshNodeDescriptor> page) {
        List<CompletableFuture<ZLinkMeshNodeDescriptor>> projections =
            page.items().stream()
                .map(descriptor -> projectMeshNodeCapacity(descriptor)
                    .toCompletableFuture())
                .toList();
        return CompletableFuture.allOf(
                projections.toArray(CompletableFuture[]::new))
            .thenApply(ignored -> new ZLinkLocationPage<>(
                projections.stream()
                    .map(projection -> projection.getNow(null))
                    .toList(),
                page.continuationToken()));
    }

    CompletionStage<byte[]> readMembershipMutation(
        String authorityKey) {
        return connection.commands()
            .thenCompose(redis -> redis.hget(
                keys.authorityMembershipsKey(),
                authorityKey))
            .thenApply(value -> value == null
                ? null
                : Base64.getDecoder().decode(value));
    }

    private CompletionStage<ZLinkMeshNodeDescriptor>
        projectMeshNodeCapacity(
            ZLinkMeshNodeDescriptor descriptor) {
        String descriptorIdentity = descriptorKey(
            new ZLinkMeshNodeDescriptorKey(
                descriptor.meshName(),
                descriptor.rid()));
        String lifecycle =
            Long.toString(descriptor.lifecycleGeneration());
        return connection.commands()
            .thenCompose(redis -> {
                var typeActive = redis.hgetall(
                    keys.capacityTypeActiveKey());
                var typeReserved = redis.hgetall(
                    keys.capacityTypePendingKey());
                var actorActive = redis.hgetall(
                    keys.capacityActorActiveKey());
                var actorReserved = redis.hgetall(
                    keys.capacityActorReservedKey());
                var spotActive = redis.hgetall(
                    keys.capacitySpotActiveKey());
                var spotReserved = redis.hgetall(
                    keys.capacitySpotReservedKey());
                return typeActive.thenCombine(
                        typeReserved,
                        CapacityMaps::new)
                    .thenCombine(
                        actorActive,
                        CapacityMaps::withActorActive)
                    .thenCombine(
                        actorReserved,
                        CapacityMaps::withActorReserved)
                    .thenCombine(
                        spotActive,
                        CapacityMaps::withSpotActive)
                    .thenCombine(
                        spotReserved,
                        CapacityMaps::withSpotReserved);
            })
            .thenApply(values -> withCapacity(
                descriptor,
                descriptorIdentity,
                lifecycle,
                values));
    }

    private static ZLinkMeshNodeDescriptor withCapacity(
        ZLinkMeshNodeDescriptor descriptor,
        String descriptorIdentity,
        String lifecycle,
        CapacityMaps values) {
        ZLinkCapacityUsage actors = usage(
            values.actorActive,
            values.actorReserved,
            populationCapacityBucket(
                descriptorIdentity, lifecycle, "actor"),
            descriptor.capacity().actors().limit());
        ZLinkCapacityUsage spots = usage(
            values.spotActive,
            values.spotReserved,
            populationCapacityBucket(
                descriptorIdentity, lifecycle, "user_spot"),
            descriptor.capacity().spots().limit());
        List<ZLinkSpotTypeCapacity> spotTypes =
            descriptor.objectCapabilities().stream()
                .filter(capability ->
                    capability.objectKind()
                        != ZLinkPlacementObjectKind.ACTOR)
                .sorted(java.util.Comparator
                    .<ZLinkObjectCapability>comparingInt(
                        capability ->
                            capability.objectKind().value())
                    .thenComparing(
                        ZLinkObjectCapability::stableType))
                .map(capability -> new ZLinkSpotTypeCapacity(
                    capability.objectKind(),
                    capability.stableType(),
                    usage(
                        values.typeActive,
                        values.typeReserved,
                        capacityBucket(
                            descriptorIdentity,
                            lifecycle,
                            objectKindToken(
                                capability.objectKind()),
                            capability.stableType()),
                        capability.spotLimit())))
                .toList();
        return new ZLinkMeshNodeDescriptor(
            descriptor.meshName(),
            descriptor.rid(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor.endpoint(),
            descriptor.channelWeights(),
            descriptor.applicationVersion(),
            descriptor.objectCapabilities(),
            descriptor.objectRole(),
            descriptor.entrySpotId(),
            descriptor.placementWeight(),
            new ZLinkPlacementCapacity(
                actors,
                spots,
                spotTypes),
            descriptor.activationConcurrency(),
            descriptor.maintenanceWave(),
            descriptor.state(),
            descriptor.securityIdentity(),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.updatedAt());
    }

    private static ZLinkCapacityUsage usage(
        Map<String, String> active,
        Map<String, String> reserved,
        String bucket,
        int limit) {
        return new ZLinkCapacityUsage(
            Math.toIntExact(Long.parseLong(
                active.getOrDefault(bucket, "0"))),
            Math.toIntExact(Long.parseLong(
                reserved.getOrDefault(bucket, "0"))),
            limit);
    }

    private static String capacityBucket(
        String descriptor,
        String lifecycle,
        String kind,
        String stableType) {
        return segment(descriptor)
            + segment(lifecycle)
            + segment(kind)
            + segment(stableType);
    }

    private static String populationCapacityBucket(
        String descriptor,
        String lifecycle,
        String kind) {
        String population = "actor".equals(kind)
            ? "actor"
            : "spot";
        return segment(descriptor)
            + segment(lifecycle)
            + segment(population);
    }

    private static String segment(String value) {
        return value.length() + ":" + value;
    }

    private ZLinkAuthorityReadResult readResult(List<Object> raw) {
        Instant now = time(raw, 1);
        if ("missing".equals(string(raw.getFirst()))) {
            return new ZLinkAuthorityMissing(now);
        }
        return snapshot(raw, 2, now, true);
    }

    private ZLinkObjectReserveResult reserveResult(
        ZLinkObjectReservationRequest request,
        List<Object> raw) {
        String status = string(raw.getFirst());
        Instant now = time(raw, 1);
        if ("generation-exhausted".equals(status)) {
            return new ZLinkObjectGenerationExhausted();
        }
        if ("owner-stale".equals(status)
            || "identity-conflict".equals(status)) {
            return new ZLinkObjectConflict(
                new ZLinkAuthorityMissing(now));
        }
        if ("capacity-exhausted".equals(status)) {
            return new ZLinkPlacementCapacityExhausted();
        }
        if ("already-exists".equals(status)
            || "type-mismatch".equals(status)
            || "creating-conflict".equals(status)) {
            ZLinkAuthoritySnapshot current =
                snapshot(raw, 2, now, true);
            if ("creating-conflict".equals(status)) {
                return new ZLinkObjectConflict(current);
            }
            return "already-exists".equals(status)
                ? new ZLinkObjectAlreadyExists(current)
                : new ZLinkObjectTypeMismatch(current);
        }
        if (!"reserved".equals(status)) {
            throw new IllegalStateException("unknown reserve result: " + status);
        }
        return new ZLinkObjectReserved(new ZLinkObjectReservation(
            request.authorityKey(),
            string(raw.get(2)),
            number(raw.get(3)),
            number(raw.get(4)),
            string(raw.get(5)),
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            request.targetOwner()));
    }

    private ZLinkAuthoritySnapshot snapshot(
        List<Object> raw,
        int offset,
        Instant now,
        boolean includesPendingCreation) {
        java.util.Optional<ZLinkPendingObjectCreation> pending =
            java.util.Optional.empty();
        if (includesPendingCreation && raw.size() >= offset + 16) {
            String reservationId = string(raw.get(offset + 12));
            if (!reservationId.isEmpty()) {
                pending = java.util.Optional.of(
                    new ZLinkPendingObjectCreation(
                        reservationId,
                        string(raw.get(offset + 13)),
                        decode(string(raw.get(offset + 14))),
                        Math.toIntExact(number(raw.get(offset + 15)))));
            }
        }
        return new ZLinkAuthoritySnapshot(
            string(raw.get(offset)),
            decode(string(raw.get(offset + 1))),
            number(raw.get(offset + 2)),
            number(raw.get(offset + 3)),
            string(raw.get(offset + 4)),
            number(raw.get(offset + 5)),
            allocation(raw, offset + 6),
            pending,
            now);
    }

    private ZLinkAuthorityScanResult toPage(List<Object> raw) {
        if ("expired".equals(string(raw.getFirst()))) {
            return new ZLinkAuthorityScanExpired();
        }
        Instant now = time(raw, 1);
        java.util.UUID scanId = uuidFromHex(string(raw.get(2)));
        String watermark = string(raw.get(3));
        int nextOffset = Math.toIntExact(number(raw.get(4)));
        @SuppressWarnings("unchecked")
        List<Object> values = (List<Object>) raw.get(5);
        List<ZLinkAuthorityEntry> items = new ArrayList<>(values.size() / 13);
        for (int index = 0; index + 12 < values.size(); index += 13) {
            items.add(new ZLinkAuthorityEntry(
                string(values.get(index)),
                snapshot(values, index + 1, now, false)));
        }
        Optional<ZLinkAuthorityScanCursor> next = nextOffset < 0
            ? Optional.empty()
            : Optional.of(encodeCursor(
                new Cursor(scanId, watermark, nextOffset)));
        return new ZLinkAuthorityPage(items, next);
    }

    private String authorityRowPrefix() {
        return keys.authorityRowKeyPrefix();
    }

    private ZLinkAuthorityScanCursor encodeCursor(Cursor cursor) {
        String raw = uuidHex(cursor.scanId)
            + ":"
            + cursor.watermark
            + ":"
            + cursor.offset;
        return new ZLinkAuthorityScanCursor(Base64.getUrlEncoder()
            .withoutPadding()
            .encodeToString(raw.getBytes(StandardCharsets.UTF_8)));
    }

    private Cursor decodeCursor(ZLinkAuthorityScanCursor cursor) {
        try {
            String raw = new String(
                Base64.getUrlDecoder().decode(cursor.encoded()),
                StandardCharsets.UTF_8);
            String[] parts = raw.split(":", -1);
            if (parts.length != 3) {
                throw new IllegalArgumentException(
                    "invalid authority scan cursor fields");
            }
            return new Cursor(
                uuidFromHex(parts[0]),
                parts[1],
                Integer.parseInt(parts[2]));
        } catch (RuntimeException error) {
            throw new IllegalArgumentException("invalid authority scan cursor", error);
        }
    }

    private static void requireKey(String key) {
        if (key == null || key.isBlank()) {
            throw new IllegalArgumentException("authority key is required");
        }
    }

    private static boolean cancelled(ZLinkStoreCancellation cancellation) {
        return java.util.Objects.requireNonNull(cancellation, "cancellation")
            .isCancellationRequested();
    }

    private static <T> CompletionStage<T> cancelledStage() {
        return CompletableFuture.failedFuture(
            new java.util.concurrent.CancellationException(
                "store operation was cancelled before I/O"));
    }

    private static String encode(byte[] value) {
        return Base64.getEncoder().encodeToString(value.clone());
    }

    static String encodeCapacityBundle(
        ZLinkPlacementCapacityBundle bundle) {
        StringBuilder encoded = new StringBuilder();
        appendCapacitySegment(encoded, "zlink-capacity-bundle-v2");
        appendCapacitySegment(
            encoded,
            Integer.toString(bundle.actorSlots()));
        appendCapacitySegment(
            encoded,
            Integer.toString(bundle.spotSlots()));
        appendCapacitySegment(
            encoded,
            bundle.spotType().isPresent() ? "1" : "0");
        bundle.spotType().ifPresent(delta -> {
            appendCapacitySegment(
                encoded,
                objectKindToken(delta.objectKind()));
            appendCapacitySegment(encoded, delta.stableType());
            appendCapacitySegment(
                encoded,
                Integer.toString(delta.slots()));
        });
        return encoded.toString();
    }

    static ZLinkPlacementCapacityBundle decodeCapacityBundle(
        String encoded) {
        List<String> segments = new ArrayList<>();
        int offset = 0;
        while (offset < encoded.length()) {
            int colon = encoded.indexOf(':', offset);
            if (colon <= offset) {
                throw new IllegalStateException(
                    "invalid capacity bundle segment length");
            }
            int length;
            try {
                length = Integer.parseInt(
                    encoded.substring(offset, colon));
            } catch (NumberFormatException error) {
                throw new IllegalStateException(
                    "invalid capacity bundle segment length",
                    error);
            }
            int start = colon + 1;
            int end = start;
            int bytes = 0;
            while (end < encoded.length() && bytes < length) {
                int codePoint = encoded.codePointAt(end);
                bytes += new String(Character.toChars(codePoint))
                    .getBytes(StandardCharsets.UTF_8).length;
                end += Character.charCount(codePoint);
            }
            if (bytes != length) {
                throw new IllegalStateException(
                    "capacity bundle segment is truncated");
            }
            segments.add(encoded.substring(start, end));
            offset = end;
        }
        if (segments.size() < 4
            || !"zlink-capacity-bundle-v2".equals(segments.get(0))) {
            throw new IllegalStateException(
                "unsupported capacity bundle domain");
        }
        int actors = parseCapacityInteger(segments.get(1));
        int spots = parseCapacityInteger(segments.get(2));
        String presence = segments.get(3);
        Optional<ZLinkSpotTypeCapacityDelta> spotType;
        if ("0".equals(presence) && segments.size() == 4) {
            spotType = Optional.empty();
        } else if ("1".equals(presence) && segments.size() == 7) {
            spotType = Optional.of(new ZLinkSpotTypeCapacityDelta(
                objectKind(segments.get(4)),
                segments.get(5),
                parseCapacityInteger(segments.get(6))));
        } else {
            throw new IllegalStateException(
                "invalid capacity bundle spot type presence");
        }
        return new ZLinkPlacementCapacityBundle(
            actors,
            spots,
            spotType);
    }

    private static void appendCapacitySegment(
        StringBuilder encoded,
        String value) {
        encoded.append(
            value.getBytes(StandardCharsets.UTF_8).length);
        encoded.append(':').append(value);
    }

    private static int parseCapacityInteger(String value) {
        try {
            int parsed = Integer.parseInt(value);
            if (parsed < 0
                || (!"0".equals(value)
                    && value.startsWith("0"))) {
                throw new NumberFormatException();
            }
            return parsed;
        } catch (NumberFormatException error) {
            throw new IllegalStateException(
                "capacity bundle integer is invalid",
                error);
        }
    }

    private static String descriptorKey(
        ZLinkMeshNodeDescriptorKey descriptor) {
        return ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(descriptor);
    }

    private static String relocationSignature(
        ZLinkRelocationCapacityReservationRequest request) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (DataOutputStream data = new DataOutputStream(bytes)) {
                writeField(data, request.reservationId().toString());
                writeField(data, request.authorityKey());
                writeField(data, request.expectedStoreVersion());
                data.writeInt(request.objectKind().value());
                writeField(data, request.stableType());
                writeField(
                    data,
                    descriptorKey(request.sourceDescriptor()));
                data.writeLong(
                    request.sourceDescriptorLifecycleGeneration());
                writeField(data, request.sourceOwner().ownerId());
                data.writeLong(
                    request.sourceOwner().leaseGeneration());
                writeField(
                    data,
                    descriptorKey(request.targetDescriptor()));
                data.writeLong(
                    request.targetDescriptorLifecycleGeneration());
                writeField(data, request.targetOwner().ownerId());
                data.writeLong(
                    request.targetOwner().leaseGeneration());
                writeField(
                    data,
                    encodeCapacityBundle(request.capacityBundle()));
            }
            return HexFormat.of().formatHex(
                MessageDigest.getInstance("SHA-256")
                    .digest(bytes.toByteArray()));
        } catch (IOException impossible) {
            throw new IllegalStateException(
                "failed to encode relocation reservation",
                impossible);
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(
                "SHA-256 is unavailable",
                impossible);
        }
    }

    private AggregateEncoding encodeAggregate(
        ZLinkAggregatePrepareRequest request) {
        if (request.aggregateId().getMostSignificantBits() == 0L
            && request.aggregateId().getLeastSignificantBits() == 0L) {
            throw new IllegalArgumentException(
                "aggregateId must not be zero");
        }
        if (request.aggregateGeneration() <= 0
            || request.participants().isEmpty()
            || request.participants().size() > 1024
            || request.inventoryDigest().length != 32
            || request.targetOwner().leaseGeneration() <= 0) {
            throw new IllegalArgumentException(
                "aggregate generation, participants, digest or owner "
                    + "is outside the supported contract");
        }
        String previousKey = null;
        for (ZLinkAggregateParticipant participant :
            request.participants()) {
            requireKey(participant.authorityKey());
            if (previousKey != null
                && previousKey.compareTo(
                    participant.authorityKey()) >= 0) {
                throw new IllegalArgumentException(
                    "aggregate participants must be canonical, "
                        + "sorted and unique");
            }
            previousKey = participant.authorityKey();
        }

        ObjectNode root = JSON.createObjectNode();
        root.put("AggregateId", request.aggregateId().toString());
        root.put(
            "AggregateGeneration",
            request.aggregateGeneration());
        root.put(
            "InventoryDigest",
            encode(request.inventoryDigest()));
        root.put(
            "TargetOwner",
            request.targetOwner().ownerId());
        root.put(
            "TargetLease",
            request.targetOwner().leaseGeneration());
        root.put(
            "TargetDescriptor",
            descriptorKey(request.targetDescriptor()));
        root.put(
            "TargetDescriptorLifecycleGeneration",
            Long.toString(
                request.targetDescriptorLifecycleGeneration()));
        root.put(
            "CapacityBundle",
            encodeCapacityBundle(request.capacityBundle()));
        ArrayNode participants = JSON.createArrayNode();
        List<String> scriptKeys = new ArrayList<>();
        scriptKeys.add(keys.authorityAggregateKey(
            request.aggregateId(),
            request.aggregateGeneration()));
        scriptKeys.add(keys.leaseKey(
            request.targetOwner().ownerId()));
        scriptKeys.add(keys.capacityTypeActiveKey());
        scriptKeys.add(keys.counterKey());
        scriptKeys.add(keys.counterKey());
        scriptKeys.add(keys.authorityIndexKey());
        scriptKeys.add(keys.authorityMembershipsKey());
        scriptKeys.add(keys.capacityTypePendingKey());
        scriptKeys.add(keys.capacityNodeActiveKey());
        scriptKeys.add(keys.capacityNodePendingKey());
        scriptKeys.add(keys.scansWatermarkKey());
        scriptKeys.add(keys.scansExpiryKey());
        scriptKeys.add(keys.meshNodeDescriptorRowKey(
            request.targetDescriptor()));
        int targetDescriptorKeyIndex = scriptKeys.size();
        scriptKeys.add(keys.meshNodeDescriptorMetadataKey(
            request.targetDescriptor()));
        int targetAdmissionKeyIndex = scriptKeys.size();
        scriptKeys.add(keys.capacitySpotActiveKey());
        scriptKeys.add(keys.capacitySpotReservedKey());
        for (ZLinkAggregateParticipant participant :
            request.participants()) {
            ObjectNode encoded = JSON.createObjectNode();
            encoded.put(
                "AuthorityKey",
                participant.authorityKey());
            encoded.put(
                "EncodedAuthorityKey",
                keys.encodedAuthorityKey(
                    participant.authorityKey()));
            scriptKeys.add(keys.authorityRowKey(
                participant.authorityKey()));
            encoded.put("AuthorityKeyIndex", scriptKeys.size());
            scriptKeys.add(keys.authorityHistoryKey(
                participant.authorityKey()));
            encoded.put("HistoryKeyIndex", scriptKeys.size());
            scriptKeys.add(keys.authorityHistoryRevisionsKey(
                participant.authorityKey()));
            encoded.put(
                "HistoryRevisionsKeyIndex",
                scriptKeys.size());
            scriptKeys.add(keys.membershipHistoryKey(
                participant.authorityKey()));
            encoded.put(
                "MembershipHistoryKeyIndex",
                scriptKeys.size());
            scriptKeys.add(keys.membershipHistoryRevisionsKey(
                participant.authorityKey()));
            encoded.put(
                "MembershipHistoryRevisionsKeyIndex",
                scriptKeys.size());
            encoded.put(
                "ExpectedVersion",
                participant.expectedStoreVersion());
            encoded.put(
                "Transition",
                participant.ownerTransition()
                        == ZLinkAuthorityGenerationTransition
                        .NEW_OWNER
                    ? "new-owner"
                    : "preserve");
            encoded.put(
                "Payload",
                encode(participant.authorityPayload()));
            encoded.put(
                "MembershipMutation",
                encode(participant.membershipMutation()));
            if (participant.ownerTransition()
                == ZLinkAuthorityGenerationTransition.NEW_OWNER) {
                encoded.put(
                    "TargetDescriptorKeyIndex",
                    targetDescriptorKeyIndex);
                encoded.put(
                    "TargetAdmissionKeyIndex",
                    targetAdmissionKeyIndex);
                encoded.put(
                    "TargetDescriptor",
                    descriptorKey(request.targetDescriptor()));
            } else {
                encoded.putNull("TargetDescriptorKeyIndex");
                encoded.putNull("TargetDescriptor");
            }
            participants.add(encoded);
        }
        root.set("Participants", participants);
        try {
            String json = JSON.writeValueAsString(root);
            byte[] bytes = json.getBytes(StandardCharsets.UTF_8);
            if (bytes.length > 1024 * 1024) {
                throw new IllegalArgumentException(
                    "encoded aggregate request exceeds 1 MiB");
            }
            String signature = HexFormat.of().formatHex(
                MessageDigest.getInstance("SHA-256").digest(bytes));
            return new AggregateEncoding(
                json,
                signature,
                List.copyOf(scriptKeys));
        } catch (JsonProcessingException error) {
            throw new IllegalArgumentException(
                "aggregate request cannot be encoded",
                error);
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(
                "SHA-256 is unavailable",
                impossible);
        }
    }

    private String[] aggregateOperationKeys(
        String aggregateKey,
        Map<String, String> fields) {
        String targetOwner = fields.getOrDefault(
            "targetOwner",
            "");
        List<String> operationKeys = new ArrayList<>();
        operationKeys.add(aggregateKey);
        operationKeys.add(keys.leaseKey(targetOwner));
        operationKeys.add(keys.capacityTypeActiveKey());
        operationKeys.add(keys.counterKey());
        operationKeys.add(keys.counterKey());
        operationKeys.add(keys.authorityIndexKey());
        operationKeys.add(keys.authorityMembershipsKey());
        operationKeys.add(keys.capacityTypePendingKey());
        operationKeys.add(keys.capacityNodeActiveKey());
        operationKeys.add(keys.capacityNodePendingKey());
        operationKeys.add(keys.scansWatermarkKey());
        operationKeys.add(keys.scansExpiryKey());
        String record = fields.get("record");
        if (record == null) {
            return operationKeys.toArray(String[]::new);
        }
        try {
            com.fasterxml.jackson.databind.JsonNode root =
                JSON.readTree(record);
            ZLinkMeshNodeDescriptorKey targetDescriptor = descriptor(
                root.path("TargetDescriptor").asText());
            operationKeys.add(
                keys.meshNodeDescriptorRowKey(targetDescriptor));
            operationKeys.add(
                keys.meshNodeDescriptorMetadataKey(targetDescriptor));
            operationKeys.add(keys.capacitySpotActiveKey());
            operationKeys.add(keys.capacitySpotReservedKey());
            for (com.fasterxml.jackson.databind.JsonNode participant :
                root.path("Participants")) {
                operationKeys.add(keys.authorityRowKey(
                    participant.path("AuthorityKey").asText()));
                operationKeys.add(keys.authorityHistoryKey(
                    participant.path("AuthorityKey").asText()));
                operationKeys.add(keys.authorityHistoryRevisionsKey(
                    participant.path("AuthorityKey").asText()));
                operationKeys.add(keys.membershipHistoryKey(
                    participant.path("AuthorityKey").asText()));
                operationKeys.add(
                    keys.membershipHistoryRevisionsKey(
                        participant.path("AuthorityKey").asText()));
            }
            return operationKeys.toArray(String[]::new);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                "stored aggregate record is invalid",
                error);
        }
    }

    private static void writeField(
        DataOutputStream data,
        String value) throws IOException {
        byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
        data.writeInt(encoded.length);
        data.write(encoded);
    }

    private static ZLinkPlacementAllocation allocation(
        List<Object> values,
        int offset) {
        return new ZLinkPlacementAllocation(
            "pending".equals(string(values.get(offset)))
                ? ZLinkPlacementAllocationState.PENDING
                : ZLinkPlacementAllocationState.ACTIVE,
            objectKind(string(values.get(offset + 1))),
            string(values.get(offset + 2)),
            descriptor(string(values.get(offset + 3))),
            number(values.get(offset + 4)),
            decodeCapacityBundle(string(values.get(offset + 5))));
    }

    private static ZLinkPlacementObjectKind objectKind(String value) {
        return switch (value) {
            case "actor" -> ZLinkPlacementObjectKind.ACTOR;
            case "user_spot" -> ZLinkPlacementObjectKind.USER_SPOT;
            case "instance_spot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
            default -> throw new IllegalStateException(
                "unsupported placement object kind: " + value);
        };
    }

    private static String objectKindToken(
        ZLinkPlacementObjectKind value) {
        return switch (value) {
            case ACTOR -> "actor";
            case USER_SPOT -> "user_spot";
            case INSTANCE_SPOT -> "instance_spot";
            default -> throw new IllegalArgumentException(
                "unsupported placement object kind: " + value);
        };
    }

    private static ZLinkMeshNodeDescriptorKey descriptor(String value) {
        return ZLinkRedisLocationKeyCodec.decodeMeshNodeKey(value);
    }

    private static byte[] decode(String value) {
        return Base64.getDecoder().decode(value);
    }

    private static ZLinkObjectAbortResult abortResult(String status) {
        return switch (status) {
            case "aborted" -> ZLinkObjectAbortResult.ABORTED;
            case "already-aborted" ->
                ZLinkObjectAbortResult.ALREADY_ABORTED;
            default -> ZLinkObjectAbortResult.STALE;
        };
    }

    private static String terminalState(
        ZLinkCreationTerminalState state) {
        return switch (state) {
            case CREATED -> "created";
            case REJECTED -> "rejected";
            case FAILED -> "failed";
        };
    }

    private static void validateTerminal(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkCreationTerminalState expectedState) {
        if (!reservation.equals(terminal.reservation())) {
            throw new IllegalArgumentException(
                "terminal reservation must match the exact reservation");
        }
        if (terminal.state() != expectedState) {
            throw new IllegalArgumentException(
                "terminal state must be " + expectedState);
        }
        byte[] digest;
        try {
            digest = MessageDigest.getInstance("SHA-256")
                .digest(terminal.terminalEnvelope());
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
        if (!java.util.Arrays.equals(
                digest,
                terminal.terminalSha256())) {
            throw new IllegalArgumentException(
                "terminalSha256 does not match terminalEnvelope");
        }
    }

    private static ZLinkCreationTerminalReadResult decodeCreationTerminal(
        ZLinkCreationOperationIdentity operation,
        Map<String, String> fields) {
        if (fields.isEmpty()) {
            return new ZLinkCreationTerminalMissing();
        }
        ZLinkCreationTerminalState state = switch (
            fields.getOrDefault("state", "")) {
            case "created" -> ZLinkCreationTerminalState.CREATED;
            case "rejected" -> ZLinkCreationTerminalState.REJECTED;
            case "failed" -> ZLinkCreationTerminalState.FAILED;
            default -> throw new IllegalStateException(
                "invalid creation terminal state");
        };
        ZLinkObjectReservation reservation = new ZLinkObjectReservation(
            fields.get("authorityKey"),
            fields.get("storeVersion"),
            Long.parseLong(fields.get("objectGeneration")),
            Long.parseLong(fields.get("authorityOwnerGeneration")),
            fields.get("reservationId"),
            descriptor(fields.get("targetDescriptor")),
            Long.parseLong(fields.get("targetLifecycle")),
            new ZLinkLocationOwnerToken(
                fields.get("targetOwner"),
                Long.parseLong(fields.get("targetLease"))));
        return new ZLinkCreationTerminalFound(
            new ZLinkCreationOperationTerminal(
                operation,
                reservation,
                state,
                decode(fields.get("terminalEnvelope")),
                decode(fields.get("terminalSha256")),
                Instant.ofEpochMilli(
                    Long.parseLong(fields.get("expiresAtMs")))));
    }

    private static Instant time(List<Object> values, int index) {
        return Instant.ofEpochMilli(number(values.get(index)));
    }

    private static long number(Object value) {
        if (value instanceof Number number) {
            return number.longValue();
        }
        return Long.parseLong(string(value));
    }

    private static String string(Object value) {
        if (value instanceof byte[] bytes) {
            return new String(bytes, StandardCharsets.UTF_8);
        }
        return value == null ? "" : value.toString();
    }

    private static String fixedHex(String decimal) {
        return String.format(
            java.util.Locale.ROOT,
            "%016x",
            new java.math.BigInteger(decimal));
    }

    private static String uuidHex(java.util.UUID value) {
        return value.toString().replace("-", "")
            .toLowerCase(java.util.Locale.ROOT);
    }

    private static java.util.UUID uuidFromHex(String value) {
        if (value.length() != 32) {
            throw new IllegalArgumentException("invalid scan id");
        }
        return java.util.UUID.fromString(
            value.substring(0, 8)
                + "-"
                + value.substring(8, 12)
                + "-"
                + value.substring(12, 16)
                + "-"
                + value.substring(16, 20)
                + "-"
                + value.substring(20));
    }

    private record Cursor(
        java.util.UUID scanId,
        String watermark,
        int offset) {
    }

    private record CasInputs(
        String leaseOwner,
        Optional<ZLinkMeshNodeDescriptorKey> targetDescriptor) {
    }

    private record CapacityMaps(
        Map<String, String> typeActive,
        Map<String, String> typeReserved,
        Map<String, String> actorActive,
        Map<String, String> actorReserved,
        Map<String, String> spotActive,
        Map<String, String> spotReserved) {
        private CapacityMaps(
            Map<String, String> typeActive,
            Map<String, String> typeReserved) {
            this(
                typeActive,
                typeReserved,
                Map.of(),
                Map.of(),
                Map.of(),
                Map.of());
        }

        private static CapacityMaps withActorActive(
            CapacityMaps values,
            Map<String, String> actorActive) {
            return new CapacityMaps(
                values.typeActive,
                values.typeReserved,
                actorActive,
                Map.of(),
                Map.of(),
                Map.of());
        }

        private static CapacityMaps withActorReserved(
            CapacityMaps values,
            Map<String, String> actorReserved) {
            return new CapacityMaps(
                values.typeActive,
                values.typeReserved,
                values.actorActive,
                actorReserved,
                Map.of(),
                Map.of());
        }

        private static CapacityMaps withSpotActive(
            CapacityMaps values,
            Map<String, String> spotActive) {
            return new CapacityMaps(
                values.typeActive,
                values.typeReserved,
                values.actorActive,
                values.actorReserved,
                spotActive,
                Map.of());
        }

        private static CapacityMaps withSpotReserved(
            CapacityMaps values,
            Map<String, String> spotReserved) {
            return new CapacityMaps(
                values.typeActive,
                values.typeReserved,
                values.actorActive,
                values.actorReserved,
                values.spotActive,
                spotReserved);
        }
    }

    private record AggregateEncoding(
        String json,
        String signature,
        List<String> keys) {
    }

}
