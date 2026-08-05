package systems.zlink.e2e.kotlin.registrymessaging.client

import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmA1DiscoveryRequestScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmA2ManualEndpointScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmA4SameRidFailoverScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmA6MultipleChannelsScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmB1ScaleOutScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmB2ScaleInScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC1RequestSendScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC2TargetedRouteScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC3MultiProviderDistributionScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC4TimeoutIsolationScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC5MissingPacketScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC7WeightedProviderScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC8PayloadRoundTripScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios.RmC9BackpressureScenario
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ClientOptions
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson

fun main(args: Array<String>) {
    val options = ClientOptions.parse(args)
    HttpJson(options.providerAUrl).use { providerA ->
        HttpJson(options.providerBUrl).use { providerB ->
            HttpJson(options.workflowUrl).use { workflow ->
                HttpJson(options.directConsumerUrl).use { directConsumer ->
                    HttpJson(options.singleConsumerUrl).use { singleConsumer ->
                        HttpJson(options.discoveryConsumerUrl).use { discoveryConsumer ->
                            HttpJson(options.backpressureConsumerUrl).use { backpressureConsumer ->
                                val scenarios = linkedMapOf<String, () -> Unit>(
                                    "RM-A1" to { RmA1DiscoveryRequestScenario.run(providerA, providerB, discoveryConsumer) },
                                    "RM-A2" to { RmA2ManualEndpointScenario.run(singleConsumer, providerA) },
                                    "RM-A4" to { RmA4SameRidFailoverScenario.run(options) },
                                    "RM-A6" to { RmA6MultipleChannelsScenario.run(discoveryConsumer, providerA, providerB, workflow) },
                                    "RM-B1" to { RmB1ScaleOutScenario.run(options) },
                                    "RM-B2" to { RmB2ScaleInScenario.run(options) },
                                    "RM-C1" to { RmC1RequestSendScenario.run(providerA, providerB) },
                                    "RM-C2" to { RmC2TargetedRouteScenario.run(providerA, providerB) },
                                    "RM-C3" to { RmC3MultiProviderDistributionScenario.run(directConsumer, providerA, providerB) },
                                    "RM-C4" to { RmC4TimeoutIsolationScenario.run(discoveryConsumer, providerA, providerB) },
                                    "RM-C5" to { RmC5MissingPacketScenario.run(discoveryConsumer, providerA, providerB) },
                                    "RM-C7" to { RmC7WeightedProviderScenario.run(options) },
                                    "RM-C8" to { RmC8PayloadRoundTripScenario.run(singleConsumer, providerA, providerB) },
                                    "RM-C9" to { RmC9BackpressureScenario.run(backpressureConsumer, providerA, providerB) },
                                )
                                if (options.scenario.equals("all", ignoreCase = true)) {
                                    scenarios.values.forEach { it.invoke() }
                                } else {
                                    val scenario = scenarios[options.scenario.uppercase()]
                                        ?: throw IllegalArgumentException("Unknown scenario '${options.scenario}'.")
                                    scenario.invoke()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    println("registry-messaging kotlin e2e result=passed")
}
