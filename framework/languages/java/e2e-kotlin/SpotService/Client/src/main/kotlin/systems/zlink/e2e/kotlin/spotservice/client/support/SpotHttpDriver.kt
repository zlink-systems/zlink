package systems.zlink.e2e.kotlin.spotservice.client.support

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env

internal class SpotHttpDriver(
    private val playA: String = Env.get("e2e.http.a.endpoint"),
    private val playB: String = Env.get("e2e.http.b.endpoint"),
) {
    suspend fun requestState(
        spotRid: String,
        op: String,
        timeoutMilliseconds: Int = 5_000,
        packetName: String = "StateReq"
    ): Contracts.StateRes {
        return postJson(
            endpointFor(spotRid),
            "/spot/state/request",
            Contracts.SpotStateRouteReq(spotRid, op, timeoutMilliseconds, packetName),
            Contracts.StateRes::class.java
        )
    }

    suspend fun sendState(
        spotRid: String,
        value: String,
        packetName: String = "StateMsg"
    ): Contracts.AckRes {
        return postJson(
            endpointFor(spotRid),
            "/spot/state/command",
            Contracts.SpotStateCommandReq(spotRid, value, packetName),
            Contracts.AckRes::class.java
        )
    }

    suspend fun requestStage(
        spotRid: String,
        marker: String,
        op: String
    ): Contracts.StateRes =
        postJson(
            endpointFor(spotRid),
            "/spot/stage/request",
            Contracts.SpotStageProbeReq(spotRid, marker, op),
            Contracts.StateRes::class.java
        )

    suspend fun startStageTimer(
        spotRid: String,
        name: String,
        periodMilliseconds: Int
    ): Contracts.SpotStageTimerRes =
        postJson(
            endpointFor(spotRid),
            "/spot/stage/timer",
            Contracts.SpotStageTimerReq(spotRid, name, periodMilliseconds),
            Contracts.SpotStageTimerRes::class.java
        )

    suspend fun requestSlow(
        spotRid: String,
        value: String,
        timeoutMilliseconds: Int
    ): Contracts.StateRes {
        return postJson(
            endpointFor(spotRid),
            "/spot/slow/request",
            Contracts.SpotSlowRouteReq(spotRid, value, timeoutMilliseconds),
            Contracts.StateRes::class.java
        )
    }

    suspend fun requestOutbound(spotRid: String, value: String): Contracts.OutboundRes {
        return postJson(
            endpointFor(spotRid),
            "/spot/outbound/request",
            Contracts.SpotOutboundRouteReq(spotRid, value),
            Contracts.OutboundRes::class.java
        )
    }

    suspend fun sendOutbound(
        spotRid: String,
        value: String,
        packetName: String = "OutboundMsg"
    ): Contracts.AckRes {
        return postJson(
            endpointFor(spotRid),
            "/spot/outbound/command",
            Contracts.SpotOutboundCommandReq(spotRid, value, packetName),
            Contracts.AckRes::class.java
        )
    }

    suspend fun routePing(targetRid: String, value: String): Contracts.RoutePingRes {
        return postJson(
            if (targetRid == "play-a") playB else playA,
            "/route/ping",
            Contracts.RoutePingHttpReq(targetRid, value),
            Contracts.RoutePingRes::class.java
        )
    }

    private fun endpointFor(spotRid: String): String =
        if (spotRid == "room-b") playB else playA
}
