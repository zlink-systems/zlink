package systems.zlink.e2e.kotlin.runtimemonitoring.client.scenarios

import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.charset.StandardCharsets
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ClientOptions
import systems.zlink.e2e.kotlin.runtimemonitoring.client.MonitoringEvidenceClient
import systems.zlink.e2e.kotlin.runtimemonitoring.client.ScenarioAssert

class MonA5FixedKindsScenario(
    private val options: ClientOptions,
    private val evidence: MonitoringEvidenceClient,
) {
    fun run() {
        evidence.waitForAnyEvent(options.serviceHttp, "socket", setOf("CONNECTED", "CONNECTION_READY"))
        triggerHandshakeFailure()
        evidence.waitForEvent(
            options.serviceHttp,
            "socket",
            Contracts.HANDSHAKE_CHANNEL,
            setOf("DISCONNECTED"),
        )
        println("scenario MON-A5 passed")
    }

    private fun triggerHandshakeFailure() {
        val port = options.handshakeEndpoint.substringAfterLast(':').toInt()
        repeat(5) { index ->
            try {
                Socket().use { socket ->
                    socket.connect(InetSocketAddress("127.0.0.1", port), 500)
                    socket.getOutputStream().use { output ->
                        output.write("invalid-zmtp-handshake-$index".toByteArray(StandardCharsets.UTF_8))
                        output.flush()
                    }
                }
            } catch (_: IOException) {
                // The server is expected to reject the malformed handshake.
            }
            ScenarioAssert.sleep(100)
        }
    }
}
