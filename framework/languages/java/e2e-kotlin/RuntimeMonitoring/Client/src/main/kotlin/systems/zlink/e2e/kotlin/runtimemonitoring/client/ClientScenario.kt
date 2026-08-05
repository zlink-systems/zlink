package systems.zlink.e2e.kotlin.runtimemonitoring.client

import com.fasterxml.jackson.databind.ObjectMapper
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonA1SocketEventsScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonA2RegistryEventsScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonA3SpotEventsScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonA4AvailabilityTransitionScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonA5FixedKindsScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonB1KindFilterScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonC1DispatchFailureScenario
import systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios.MonD1FailureRecoveryScenario

class ClientScenario(
    json: ObjectMapper,
) {
    private val options = ClientOptions()
    private val evidence = MonitoringEvidenceClient(json)
    private val monA1 = MonA1SocketEventsScenario(options, evidence)
    private val monA2 = MonA2RegistryEventsScenario(options, evidence)
    private val monA3 = MonA3SpotEventsScenario(options, evidence)
    private val monA4 = MonA4AvailabilityTransitionScenario(options, evidence)
    private val monA5 = MonA5FixedKindsScenario(options, evidence)
    private val monB1 = MonB1KindFilterScenario(options, evidence)
    private val monC1 = MonC1DispatchFailureScenario(options, evidence)
    private val monD1 = MonD1FailureRecoveryScenario(options, evidence)

    fun run() {
        if (options.scenario != "all") {
            runOne(options.scenario)
            return
        }
        monA1.run()
        monA2.run()
        monA3.run()
        monA4.runDrainSubset()
        monA5.run()
        monB1.run()
        monC1.run()
        monD1.run()
    }

    private fun runOne(scenario: String) {
        when (scenario) {
            "MON-A1" -> monA1.run()
            "MON-A2" -> monA2.run()
            "MON-A3" -> monA3.run()
            "MON-A4" -> monA4.runDrainSubset()
            "MON-A5" -> monA5.run()
            "MON-B1" -> monB1.run()
            "MON-C1" -> monC1.run()
            "MON-D1" -> monD1.run()
            else -> throw IllegalArgumentException("Unknown RuntimeMonitoring scenario '$scenario'.")
        }
    }
}
