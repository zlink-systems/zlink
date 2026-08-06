package systems.zlink.e2e.kotlin.channelegress.shared

import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.withTimeout
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingPublishHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToChannel

object Contracts {
    const val GAME_MESH = "channel-egress.game"
    const val AUDIT_MESH = "channel-egress.audit"
    const val SESSION_CHANNEL = "game.session"
    const val PLAY_CHANNEL = "game.play"
    const val API_CHANNEL = "game.api"
    const val AUDIT_CHANNEL = "audit.record"
    const val WORKFLOW_CHANNEL = "workflow.command"
    const val FANOUT_CHANNEL = "channel-egress.fanout"
    const val STREAM_NODE = "channel-egress.stream"
    const val INSTANCE_SPOT_TYPE = "config12.workflow-spot"
    const val ACTOR_TYPE = "config12.workflow-actor"
    const val HANDLER_GROUP = "channel-egress-kotlin"
    const val STATE_HANDLER_GROUP = "channel-egress-kotlin-state-address"
    const val FANOUT_HANDLER_GROUP = "channel-egress-kotlin-fanout"

    data class ChannelProbeReq(val id: String, val mode: String = "echo")
    data class ChannelProbeRes(
        val id: String,
        val role: String,
        val lifecycle: String,
        val channel: String,
        val downstream: List<String> = emptyList(),
    )

    data class ChannelProbeMsg(val id: String)
    data class FanoutProbe(val id: String)
    data class StreamProbe(val id: String)
    data class SpotWorkflowReq(val id: String, val timerName: String = "$id-timer")
    data class SpotWorkflowRes(val id: String, val sequence: List<String>)
    data class SpotCreateReq(val spotId: String)
    data class SpotCreateRes(val spotId: String, val nodeRid: String)
    data class ActorCreateReq(val actorId: String)
    data class ActorCreateRes(val actorId: String, val nodeRid: String)
    data class ObjectProbeReq(val id: String)
    data class ObjectProbeRes(val id: String, val kind: String, val objectId: String, val role: String)
    data class StateAddressReq(val id: String, val spotId: String, val actorId: String)
    data class StateAddressRes(val id: String, val downstream: List<String>)
    data class InvokeReq(val channel: String, val id: String, val mode: String = "echo")
    data class InvokeRes(
        val succeeded: Boolean,
        val error: String? = null,
        val reply: ChannelProbeRes? = null,
        val elapsedMilliseconds: Long,
    )
    data class SendRes(val succeeded: Boolean, val error: String? = null, val elapsedMilliseconds: Long)
    data class EvidenceEntry(val marker: String, val role: String, val rid: String, val value: String)
    data class EvidenceSnapshot(val role: String, val rid: String, val entries: List<EvidenceEntry>)
    data class WorkflowStatus(
        val state: String,
        val isReady: Boolean,
        val readyTargetCount: Int,
        val localRole: String,
        val targets: List<WorkflowTarget>,
    )
    data class WorkflowTarget(val rid: String, val weight: Int, val state: String)
    data class LocationEntry(val meshName: String, val rid: String, val endpoint: String, val state: String)
    data class ListenerStatus(
        val kind: String,
        val name: String,
        val isReady: Boolean,
        val advertisedEndpoint: String?,
        val detail: String,
    )
}

class EvidenceState(
    val role: String,
    val rid: String,
) {
    private val entries = mutableListOf<Contracts.EvidenceEntry>()
    private val release = CompletableFuture<Void>()
    @Volatile private var held = false

    @Synchronized
    fun add(marker: String, value: String) {
        entries += Contracts.EvidenceEntry(marker, role, rid, value)
    }

    @Synchronized
    fun snapshot(): Contracts.EvidenceSnapshot =
        Contracts.EvidenceSnapshot(role, rid, entries.toList())

    @Synchronized
    fun hold() {
        held = true
        add("request-held", "role=$role")
    }

    fun isHeld(): Boolean = held

    suspend fun awaitRelease() {
        withTimeout(20_000) {
            release.await()
        }
    }

    fun release() {
        release.complete(null)
        add("request-released", "role=$role")
    }

    @Synchronized
    fun contains(fragment: String): Boolean = entries.any { evidenceLine(it).contains(fragment) }

    companion object {
        fun evidenceLine(entry: Contracts.EvidenceEntry): String =
            "${entry.marker}|role=${entry.role}|rid=${entry.rid}|${entry.value}"
    }
}

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class ChannelProbeRequestHandler(
    private val evidence: EvidenceState,
    routes: ZLinkRouteClient,
    channels: ZLinkClient,
) : ZLinkSuspendingRequestHandler<Contracts.ChannelProbeReq, Contracts.ChannelProbeRes> {
    private val routes = routes.kotlin()
    private val channels = channels.kotlin()

    init {
        evidence.add("handler-created", "ChannelProbeRequestHandler")
    }

    override suspend fun handle(
        request: Contracts.ChannelProbeReq,
        context: ZLinkMessageContext,
    ): Contracts.ChannelProbeRes {
        val channel = context.channelName().orElse("<none>")
        evidence.add("request-start", "channel=$channel|id=${request.id}")
        if (evidence.isHeld() && request.mode == "hold") {
            evidence.awaitRelease()
        }
        val downstream = if (
            evidence.role == "play" &&
            channel == Contracts.PLAY_CHANNEL &&
            request.mode == "cascade"
        ) {
            val audit = routes.requestToChannel<Contracts.ChannelProbeRes>(
                Contracts.AUDIT_CHANNEL,
                Contracts.ChannelProbeReq("${request.id}-audit"),
            ).await()
            val workflow = channels.requestToChannel<Contracts.ChannelProbeRes>(
                Contracts.WORKFLOW_CHANNEL,
                Contracts.ChannelProbeReq("${request.id}-workflow"),
            ).await()
            listOf("${audit.role}:${audit.channel}", "${workflow.role}:${workflow.channel}")
        } else {
            emptyList()
        }
        evidence.add("request-end", "channel=$channel|id=${request.id}")
        return Contracts.ChannelProbeRes(
            request.id,
            evidence.role,
            evidence.rid,
            channel,
            downstream,
        )
    }
}

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class ChannelProbeSendHandler(
    private val evidence: EvidenceState,
) : ZLinkSuspendingSendHandler<Contracts.ChannelProbeMsg> {
    override suspend fun handle(message: Contracts.ChannelProbeMsg, context: ZLinkMessageContext) {
        evidence.add(
            "send",
            "channel=${context.channelName().orElse("<none>")}|id=${message.id}",
        )
    }
}

@ZLinkHandlerGroup(Contracts.FANOUT_HANDLER_GROUP)
class FanoutProbeHandler(
    private val evidence: EvidenceState,
) : ZLinkSuspendingPublishHandler<Contracts.FanoutProbe> {
    override suspend fun handle(
        message: Contracts.FanoutProbe,
        context: ZLinkPublishMessageContext,
    ) {
        evidence.add("fanout", "channel=${context.channelName()}|id=${message.id}")
    }
}
