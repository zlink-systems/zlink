package systems.zlink.e2e.kotlin.runtimemonitoring

class Contracts private constructor() {
    companion object {
        const val CHANNEL: String = "monitoring.api"
        const val HANDSHAKE_CHANNEL: String = "monitoring.handshake"
        const val SPOT_MESH: String = "monitoring.spot.mesh"
        const val SPOT_CHANNEL: String = "monitoring.spot.runtime"
        const val HANDLER_GROUP: String = "monitoring"
    const val LOCATION_SOURCE: String = "ops-locations"
    const val ACTOR_TYPE: String = "monitoring-actor"
    }

    @JvmRecord
    data class WorkReq(val value: String)

    @JvmRecord
    data class WorkRes(
        val value: String,
        val providerRid: String,
    )

    @JvmRecord
    data class EvidenceEntry(
        val surface: String,
        val sourceName: String,
        val event: String,
        val detail: String,
    )

    @JvmRecord
    data class EvidenceSnapshot(val entries: List<EvidenceEntry>)

    @JvmRecord
    data class RuntimeSnapshot(
        val meshName: String,
        val state: String,
        val ready: Boolean,
        val readyPeerCount: Int,
        val sequence: Long,
        val peers: List<RuntimePeer>,
        val channels: List<RuntimeChannel>,
        val placementAvailable: Boolean,
        val activeActorCount: Int,
        val activeSpotCount: Int,
        val placementUnavailableReason: String,
    )

    @JvmRecord
    data class RuntimePeer(
        val nodeRid: String,
        val state: String,
        val unavailableReason: String,
    )

    @JvmRecord
    data class RuntimeChannel(
        val channelName: String,
        val ready: Boolean,
        val readyTargetCount: Int,
    )

    @JvmRecord
    data class ObserverStatus(
        val started: Boolean,
        val latestSequence: Long,
        val latestReady: Boolean,
        val latestReadyPeerCount: Int,
        val error: String,
    )
}
