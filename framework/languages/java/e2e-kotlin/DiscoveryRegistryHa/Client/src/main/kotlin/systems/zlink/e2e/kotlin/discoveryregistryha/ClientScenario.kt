package systems.zlink.e2e.kotlin.discoveryregistryha

import com.fasterxml.jackson.databind.ObjectMapper
import systems.zlink.e2e.kotlin.discoveryregistryha.client.Support.ClientScenarioContext

class ClientScenario(
    json: ObjectMapper,
    options: ClientOptions,
) {
    private val context = ClientScenarioContext(json, options)

    fun run() {
        when (context.options.scenario()) {
            "SF-A1" -> context.runStoreFailureBaseline()
            "SF-A2" -> context.runStoreFailurePollingFallback()
            "SF-B1" -> context.runStoreFailureFailStaticOutage()
            "SF-B1-RECOVERED" -> context.runStoreFailureRecovered("SF-B1")
            "SF-B2" -> context.runStoreFailureGraceExceeded()
            "SF-B2-RECOVERED" -> context.runStoreFailureRecoveredWithPeers("SF-B2")
            "SF-C1" -> context.runStoreFailureCrashLeaseExpiry()
            "SF-C2" -> context.runStoreFailureGracefulRemoval()
            "SF-D1" -> context.runStoreFailureShortOutageTraffic()
            "SF-D1-RECOVERED" -> context.runStoreFailureRecoveredWithPeers("SF-D1")
            "SF-D2" -> context.runStoreFailureLongOutageRecovery()
            "SF-D3-HEALTHY" -> context.runStoreFailureStatusHealthy()
            "SF-D3-OUTAGE" -> context.runStoreFailureStatusOutage()
            "SF-D3-RECOVERED" -> context.runStoreFailureStatusRecovered()
            "SF-E1" -> context.runStoreFailureStoreDelayNonBlocking()
            else -> error("unknown scenario: ${context.options.scenario()}")
        }
    }
}
