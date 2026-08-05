package systems.zlink.e2e.kotlin.spotservice.client

import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA2Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA4Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA5Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA6Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA7Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmA8Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmB1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmB3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmB5Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmB6Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmB7Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmB8Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmC1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmC2Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmC3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmC4Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD10Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD11Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD12Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD14Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD4Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD5Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD6Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD7Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD8Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD9Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmD13Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmE1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmE2Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmE3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmE4Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmF1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmF2Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmF3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmF4Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmF5Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmF6Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmG1Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmG2Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmG3Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmG4Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmQ9Scenario
import systems.zlink.e2e.kotlin.spotservice.client.scenarios.SmRemoteActorSessionScenario
import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioSupport
import systems.zlink.e2e.kotlin.spotservice.client.support.SpotHttpDriver

internal class ClientScenario(
    private val spots: SpotHttpDriver = SpotHttpDriver(),
) {
    suspend fun runMode(mode: String) {
        when (mode) {
            "state1" -> SmA1Scenario.run(spots)
            "state2" -> SmA2Scenario.run(spots)
            "send" -> SmC1Scenario.runSend(spots)
            "timeout" -> SmC1Scenario.runTimeout(spots)
            "missing" -> SmE1Scenario.run(spots)
            "normal" -> SmC1Scenario.runNormal(spots)
            "owner" -> {
                SmA3Scenario.run(spots)
                SmA4Scenario.run(spots)
            }
            "stage-wrapper" -> SmA5Scenario.run(spots)
            "lifecycle-close" -> SmA6Scenario.run()
            "type-mismatch" -> SmA7Scenario.run()
            "route-mesh" -> {
                SmF3Scenario.run(spots)
                SmF2Scenario.run(spots)
                SmF1Scenario.run(spots)
                SmF4Scenario.run(spots)
            }
            "route-lifecycle" -> {
                SmF5Scenario.run(spots)
            }
            "spot-only-mesh" -> SmF6Scenario.run()
            "actor-session" -> ActorSessionScenarioSupport.run { context ->
                SmB1Scenario.run(context)
                SmB3Scenario.run(context)
                SmB5Scenario.run()
                SmB6Scenario.run()
                SmB7Scenario.run(context)
                SmB8Scenario.run()
                SmD1Scenario.run(context)
                SmD3Scenario.run(context)
                SmD9Scenario.run(context)
                SmD4Scenario.run()
                SmD5Scenario.run()
                SmD6Scenario.run()
                SmD7Scenario.run()
                SmD8Scenario.run()
                SmD10Scenario.run()
                SmD13Scenario.run()
                SmD11Scenario.run(spots)
            }
            "stream-tls" -> SmD14Scenario.run()
            "session-transfer" -> SmD12Scenario.run()
            "remote-actor-session" -> SmRemoteActorSessionScenario.run()
            "play-crash-recovery" -> SmG1Scenario.run(spots)
            "owner-remap" -> SmG2Scenario.run()
            "join-leave-race" -> SmG3Scenario.run()
            "bound-push-load" -> SmG4Scenario.run()
            "worker" -> SmA8Scenario.run(spots)
            "spot-outbound" -> SmC2Scenario.run(spots)
            "spot-to-spot" -> SmC3Scenario.run(spots)
            "gateway-publish" -> SmC4Scenario.run()
            "timer-basic" -> SmE2Scenario.run()
            "idle-timer" -> SmE3Scenario.run()
            "timer-overrun" -> SmE4Scenario.run()
            "multi-node" -> SmQ9Scenario.run()
            else -> throw IllegalArgumentException("unknown client mode $mode")
        }
    }

}
