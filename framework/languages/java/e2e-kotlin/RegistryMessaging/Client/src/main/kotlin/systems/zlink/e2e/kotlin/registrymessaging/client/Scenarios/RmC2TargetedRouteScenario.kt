package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.RouteMissingRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ScenarioRoutePingReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ScenarioRoutePingRes

object RmC2TargetedRouteScenario {
    fun run(providerA: HttpJson, providerB: HttpJson) {
        val marker = "rm-c2-${UUID.randomUUID().toString().replace("-", "")}"
        val reply = providerA.post<ScenarioRoutePingRes>("/profile/route/request", ScenarioRoutePingReq(marker))
        ScenarioAssert.that(reply.providerRid == "api-b", "RM-C2 targeted route request should reach api-b.")
        ScenarioAssert.that(reply.value == "route:$marker", "RM-C2 route reply value mismatch.")

        val afterB = providerB.post<List<String>>("/evidence/wait", EvidenceWaitReq(marker))
        val afterA = providerA.get<List<String>>("/evidence")
        ScenarioAssert.that(
            afterB.count { it.contains("route-request|rid=api-b") && it.contains(marker) } == 1 &&
                afterA.count { it.contains("route-request|rid=api-a") && it.contains(marker) } == 0,
            "RM-C2 targeted route evidence did not match api-b only.",
        )

        val missing = providerA.post<RouteMissingRes>("/profile/route/missing", ScenarioRoutePingReq("missing"))
        ScenarioAssert.that(missing.failed, "RM-C2 missing rid request should fail.")
        println("scenario RM-C2 passed")
    }
}
