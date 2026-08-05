// Generated from service-wire-v1.schema.json. Do not edit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zlink::framework::runtime::protocol {
inline constexpr std::uint8_t magic[] = {90, 77};
inline constexpr std::uint8_t wire_major = 1;
inline constexpr const char required_capability[] = "framework-service-v11";
inline constexpr const char framework_multipart_packet_name[] = "ZLinkFrameworkMultipart";
inline constexpr const char framework_multipart_content_type[] = "application/x-zlink-multipart";
enum class command : std::uint8_t {
    hello = 1,
    admit = 2,
    reject = 3,
    update = 4,
    livenessProbe = 5,
    livenessAck = 6,
    nodeSend = 16,
    nodeRequest = 17,
    channelSend = 18,
    channelRequest = 19,
    reply = 20,
    spotSend = 21,
    spotRequest = 22,
    logicalMulticast = 23,
    actorSend = 24,
    actorRequest = 25,
    actorLookup = 26,
    actorDestroy = 27,
    actorJoin = 28,
    actorLeft = 29,
    relocationReady = 30,
    relocationData = 31,
    relocationAck = 32,
    replyRelay = 33,
    relocationSeal = 34,
    relocationComplete = 35,
    boundSessionSend = 36,
    actorJoined = 37,
    boundSessionBind = 38,
    instanceSpot = 39,
    relocationPrepare = 40,
    relocationReserved = 41,
    sessionRelocationSeal = 42,
    sessionRelocationSealed = 43,
    sessionRelocationRoute = 44,
    sessionRelocationRouted = 45,
    replyRelayAck = 46,
    userSpotCreate = 47,
    userSpotClose = 48,
    actorCreate = 49,
    messageFollow = 50,
};
enum class flag : std::uint8_t {
    metadata = 1,
    boundSession = 2,
    sourceSpotId = 4,
    extension = 8,
};
enum class framework_error_code : std::uint32_t {
    none = 0,
    actorRouteNotFound = 1,
    actorCreateFailed = 2,
    actorAlreadyExists = 3,
    actorTypeMismatch = 4,
    spotCreateFailed = 5,
    spotRouteNotFound = 6,
    spotTypeMismatch = 7,
    actorSessionNotBound = 8,
    handlerNotFound = 9,
    routeHandlerNotFound = 10,
    actorDispatchHandlerNotFound = 11,
    payloadDecodeFailed = 12,
    routeNotConnected = 13,
    requestTargetNotFound = 14,
    requestRejected = 15,
    requestProtocolError = 16,
    requestFailed = 17,
    workerQueueFull = 18,
    workerTimedOut = 19,
    workerFailed = 20,
    actorLocationStale = 21,
    actorCreateRejected = 22,
    spotGenerationStale = 33,
    spotMoving = 34,
    relocationDataLost = 35,
};
inline constexpr std::uint64_t ridBytes = 255ULL;
inline constexpr std::uint64_t shortTextBytes = 255ULL;
inline constexpr std::uint64_t endpointBytes = 4096ULL;
inline constexpr std::uint64_t blobBytes = 65535ULL;
inline constexpr std::uint64_t metadataBytes = 1024ULL;
inline constexpr std::uint64_t packetNameBytes = 255ULL;
inline constexpr std::uint64_t contentTypeBytes = 255ULL;
inline constexpr std::uint64_t relocationReferenceBytes = 4096ULL;
inline constexpr std::uint64_t creationContentReferenceBytes = 4096ULL;
inline constexpr std::uint64_t authorityStoreVersionBytes = 4096ULL;
inline constexpr std::uint64_t wireU32CompleteMessageBytes = 4294967295ULL;
inline constexpr std::uint64_t applicationPayloadAbsoluteBytes = 4294966774ULL;
inline constexpr std::uint64_t descriptorEnvelopeBytes = 1048576ULL;
inline constexpr std::uint64_t journalRecordCount = 65535ULL;
inline constexpr std::uint64_t authorityEnvelopeBytes = 1048576ULL;
inline constexpr std::uint64_t descriptorPageBytes = 4194304ULL;
inline constexpr std::uint64_t authorityScanPageBytes = 4194304ULL;
inline constexpr std::uint64_t relocationChunkBytes = 67108864ULL;
inline constexpr std::uint64_t relocationChunkEnvelopeBytes = 67108886ULL;
inline constexpr std::uint64_t relocationChunkCount = 4096ULL;
inline constexpr std::uint64_t relocationLogicalBytes = 274877906944ULL;
inline constexpr std::uint64_t relocationManifestBytes = 33554432ULL;
inline constexpr std::uint64_t relocationControlEnvelopeBytes = 1048576ULL;
inline constexpr std::uint64_t weightMax = 100ULL;
inline constexpr std::uint64_t creationIntentBytes = 1048576ULL;
inline constexpr std::uint64_t creationTerminalEnvelopeBytes = 1048576ULL;
inline constexpr std::uint64_t maintenanceAggregateParticipants = 1024ULL;
inline constexpr std::uint64_t relocationResourceParticipants = 2048ULL;
inline constexpr std::uint64_t maintenanceAggregateBytes = 1048576ULL;
inline constexpr std::uint64_t messageFollowHopCount = 8ULL;
inline constexpr std::uint64_t messageFollowMessages = 1024ULL;
inline constexpr std::uint64_t messageFollowBytes = 16777216ULL;
inline constexpr std::uint64_t routingIdCollisionAttempts = 8ULL;
inline constexpr std::uint64_t nodeActiveCapacityDefault = 10000ULL;
inline constexpr std::uint64_t nodePendingCapacityDefault = 128ULL;
inline constexpr std::uint64_t objectTypeCapacityMaximum = 2147483647ULL;
} // namespace zlink::framework::runtime::protocol
