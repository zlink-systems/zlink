package systems.zlink.e2e.kotlin.spotservice

class Contracts private constructor() {
    companion object {
        const val ROUTE_CHANNEL: String = "spot.service.route"
        const val ROUTE_PACKET: String = "RoutePingReq"
        const val EGRESS_CHANNEL: String = "spot.service.egress"
        const val INGRESS_CHANNEL: String = "spot.service.ingress"
        const val SPOT_MESH: String = "spot.service.mesh"
        const val SPOT_NODE: String = "play"
        const val MULTI_SPOT_NODE_A: String = "multi-node-a"
        const val MULTI_SPOT_NODE_B: String = "multi-node-b"
        const val MULTI_ROUTE_CHANNEL_A: String = "multi-route-a"
        const val MULTI_ROUTE_CHANNEL_B: String = "multi-route-b"
        const val CHANNEL_HANDLER_GROUP: String = "spot-service-channel-handlers"
    }

    @JvmRecord
    data class StateReq(val op: String)

    @JvmRecord
    data class StateRes(
        val spotRid: String,
        val nodeRid: String,
        val value: String
    )

    @JvmRecord
    data class StateMsg(val value: String)

    @JvmRecord
    data class StageProbeReq(
        val marker: String,
        val op: String
    )

    @JvmRecord
    data class StageTimerStartMsg(
        val name: String,
        val periodMilliseconds: Int
    )

    @JvmRecord
    data class SpotStageProbeReq(
        val spotRid: String,
        val marker: String,
        val op: String
    )

    @JvmRecord
    data class SpotStageTimerReq(
        val spotRid: String,
        val name: String,
        val periodMilliseconds: Int
    )

    @JvmRecord
    data class SpotStageTimerRes(
        val spotRid: String,
        val name: String,
        val started: Boolean
    )

    @JvmRecord
    data class SlowReq(val value: String)

    @JvmRecord
    data class OutboundReq(val value: String)

    @JvmRecord
    data class OutboundRes(
        val spotRid: String,
        val nodeRid: String,
        val channelReply: String
    )

    @JvmRecord
    data class OutboundMsg(val value: String)

    @JvmRecord
    data class MeshMsg(val value: String)

    @JvmRecord
    data class TimerActivityReq(val value: String)

    @JvmRecord
    data class TimerActivityRes(
        val spotRid: String,
        val value: String
    )

    @JvmRecord
    data class TimerStatusRes(
        val spotRid: String,
        val value: String
    )

    @JvmRecord
    data class RoutePingReq(val value: String)

    @JvmRecord
    data class RoutePingRes(
        val value: String,
        val nodeRid: String,
        val routeRid: String
    )

    @JvmRecord
    data class SpotStateRouteReq(
        val spotRid: String,
        val op: String,
        val timeoutMilliseconds: Int = 5_000,
        val packetName: String = "StateReq"
    )

    @JvmRecord
    data class SpotStateCommandReq(
        val spotRid: String,
        val value: String,
        val packetName: String = "StateMsg"
    )

    @JvmRecord
    data class MissingSpotReq(val value: String)

    @JvmRecord
    data class MissingSpotMsg(val value: String)

    @JvmRecord
    data class CreateSpotReq(
        val spotRid: String
    )

    @JvmRecord
    data class CreateSpotRes(
        val spotRid: String,
        val nodeRid: String
    )

    @JvmRecord
    data class PlacementWeightReq(val weight: Int)

    @JvmRecord
    data class PlacementWeightRes(val weight: Int)

    @JvmRecord
    data class SpotSlowRouteReq(
        val spotRid: String,
        val value: String,
        val timeoutMilliseconds: Int
    )

    @JvmRecord
    data class SpotOutboundRouteReq(
        val spotRid: String,
        val value: String
    )

    @JvmRecord
    data class SpotOutboundCommandReq(
        val spotRid: String,
        val value: String,
        val packetName: String = "OutboundMsg"
    )

    @JvmRecord
    data class SpotToSpotCommandReq(
        val targetSpotRid: String,
        val value: String
    )

    @JvmRecord
    data class RoutePingHttpReq(
        val targetRid: String,
        val value: String
    )

    @JvmRecord
    data class AckRes(
        val accepted: Boolean
    )

    @JvmRecord
    data class MultiNodeCreateSpotReq(
        val spotRid: String,
        val delta: Int
    )

    @JvmRecord
    data class MultiNodeCreateSpotRes(
        val spotRid: String,
        val nodeRid: String,
        val state: String,
        val value: Int
    )

    @JvmRecord
    data class MultiNodeStateRouteReq(
        val spotRid: String,
        val delta: Int
    )

    @JvmRecord
    data class MultiNodeStateReq(
        val operation: String,
        val delta: Int
    )

    @JvmRecord
    data class SpotOnlyMeshReq(
        val sourceSpotRid: String,
        val targetSpotRid: String,
        val marker: String
    )

    @JvmRecord
    data class SpotOnlyMeshRes(
        val sourceSpotRid: String,
        val targetSpotRid: String,
        val targetValue: Int
    )

    @JvmRecord
    data class SpotOnlyJoinReq(
        val targetSpotRid: String,
        val actorId: String,
        val marker: String
    )

    @JvmRecord
    data class SpotOnlyJoinRes(
        val accepted: Boolean,
        val actorId: String
    )

    @JvmRecord
    data class MultiNodeStateRes(
        val spotRid: String,
        val nodeRid: String,
        val value: Int
    )

    @JvmRecord
    data class EvidenceWaitReq(
        val containsAll: List<String>,
        val timeoutMilliseconds: Int = 10_000
    )

    @JvmRecord
    data class ActorProfile(
        val displayName: String,
        val level: Int,
        val tags: List<String>
    )

    @JvmRecord
    data class ActorAuthReq(
        val actorId: String,
        val profile: ActorProfile
    )

    @JvmRecord
    data class ActorRemoteAuthReq(
        val actorId: String,
        val profile: ActorProfile,
        val nodeRid: String
    )

    @JvmRecord
    data class ActorAuthRes(
        val actorId: String,
        val nodeRid: String,
        val boundCount: Int,
        val displayName: String,
        val level: Int,
        val tags: List<String>
    )

    @JvmRecord
    data class EnsureActorReq(
        val actorId: String,
        val profile: ActorProfile
    )

    @JvmRecord
    data class EnsureActorRes(
        val actorId: String,
        val nodeRid: String,
        val generation: Long
    )

    @JvmRecord
    data class MultiBindReq(
        val firstActorId: String,
        val secondActorId: String,
        val profile: ActorProfile
    )

    @JvmRecord
    data class MultiBindRes(
        val boundCount: Int
    )

    @JvmRecord
    data class DestroyActorReq(
        val actorId: String
    )

    @JvmRecord
    data class DestroyActorRes(
        val actorId: String,
        val destroyed: Boolean
    )

    @JvmRecord
    data class LeaveActorReq(
        val actorId: String
    )

    @JvmRecord
    data class LeaveActorRes(
        val actorId: String,
        val accepted: Boolean
    )

    @JvmRecord
    data class ActorJoinReq(
        val spotRid: String,
        val profile: ActorProfile,
        val tags: List<String>
    )

    @JvmRecord
    data class ActorJoinRes(
        val actorId: String,
        val spotRid: String,
        val nodeRid: String,
        val displayName: String,
        val level: Int,
        val tags: List<String>
    )

    @JvmRecord
    data class ActorEchoReq(
        val value: String,
        val seq: Int,
        val profile: ActorProfile
    )

    data class MissingActorReq(
        val value: String,
        val seq: Int,
        val profile: ActorProfile,
    )

    @JvmRecord
    data class SlowSessionReq(
        val value: String,
        val delayMilliseconds: Int
    )

    @JvmRecord
    data class SlowSessionRes(
        val value: String
    )

    @JvmRecord
    data class ActorEchoRes(
        val actorId: String,
        val spotRid: String,
        val nodeRid: String,
        val value: String,
        val requestSeq: Int,
        val handlerSeq: Int,
        val displayName: String,
        val level: Int,
        val tags: List<String>
    )

    @JvmRecord
    data class ActorPushNotify(
        val actorId: String,
        val spotRid: String,
        val value: String,
        val requestSeq: Int,
        val handlerSeq: Int
    )

    @JvmRecord
    data class EvidenceEntry(
        val marker: String,
        val nodeRid: String,
        val spotRid: String,
        val value: String?
    )

    @JvmRecord
    data class EvidenceSnapshot(
        val nodeRid: String,
        val entries: List<EvidenceEntry>
    )
}
