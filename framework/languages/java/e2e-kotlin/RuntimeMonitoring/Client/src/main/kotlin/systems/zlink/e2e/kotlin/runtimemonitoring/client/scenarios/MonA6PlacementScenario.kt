package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import com.fasterxml.jackson.databind.JsonNode
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioBlocker

class MonA6PlacementScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        val base = options.serviceHttp
        val baseline = await { snapshot(base).path("ready").asBoolean() }
        val baselineSpots = baseline.path("activeSpotCount").asInt()

        val first = evidence.postRaw("$base/runtime/placement/spot/create?id=mon-a6-spot")
        ScenarioAssert.ensure(first.first in 200..299 && first.second.contains("\"accepted\":true"), "MON-A6 Spot create failed: $first")
        await { snapshot(base).path("activeSpotCount").asInt() == baselineSpots + 1 }

        val overflow = evidence.postRaw("$base/runtime/placement/spot/create?id=mon-a6-overflow")
        ScenarioAssert.ensure(overflow.first == 409, "MON-A6 Spot capacity was not rejected: $overflow")
        val unavailable = await { !snapshot(base).path("placementAvailable").asBoolean() }
        ScenarioAssert.ensure(unavailable.path("placementUnavailableReason").asText() == "CAPACITY_EXCEEDED", "MON-A6 capacity reason mismatch: $unavailable")

        val actor = evidence.postRaw("$base/runtime/placement/actor/create?id=mon-a6-actor")
        ScenarioAssert.ensure(actor.first in 200..299 && actor.second.contains("\"accepted\":true"), "MON-A6 actor create failed: $actor")
        await { snapshot(base).path("activeActorCount").asInt() == 1 }
        val actorOverflow = evidence.postRaw("$base/runtime/placement/actor/create?id=mon-a6-actor-overflow")
        ScenarioAssert.ensure(actorOverflow.first == 409, "MON-A6 actor capacity was not rejected: $actorOverflow")

        val destroyed = evidence.postRaw("$base/runtime/placement/actor/destroy?id=mon-a6-actor")
        ScenarioAssert.ensure(destroyed.first in 200..299 && destroyed.second.contains("\"destroyed\":true"), "MON-A6 actor destroy failed: $destroyed")
        await { snapshot(base).path("activeActorCount").asInt() == 0 }

        val replacement = evidence.postRaw("$base/runtime/placement/spot/create?id=mon-a6-replacement")
        ScenarioAssert.ensure(replacement.first in 200..299, "MON-A6 replacement Spot failed: $replacement")
        await { snapshot(base).path("activeSpotCount").asInt() == baselineSpots + 2 }
        evidence.postRaw("$base/runtime/placement/spot/close?id=mon-a6-spot")
        evidence.postRaw("$base/runtime/placement/spot/close?id=mon-a6-replacement")
        await { snapshot(base).path("activeSpotCount").asInt() == baselineSpots && snapshot(base).path("placementAvailable").asBoolean() }
        println("scenario MON-A6 passed")
    }

    private fun snapshot(base: String) = evidence.json("$base/runtime/snapshot")

    private fun await(predicate: () -> Boolean): JsonNode {
        var last = snapshot(options.serviceHttp)
        repeat(200) {
            if (predicate()) return snapshot(options.serviceHttp)
            last = snapshot(options.serviceHttp)
            ScenarioAssert.sleep(100)
        }
        throw ScenarioBlocker("public /runtime/snapshot activeSpotCount did not reflect create; last=$last")
    }
}
