package systems.zlink.e2e.kotlin.registrationcodec

import com.fasterxml.jackson.databind.ObjectMapper
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.AutoRegistrationScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.AttributeRegistrationScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.CodecMismatchScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.ManualRegistrationScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcA4DiLifecycleScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcA5FilterOrderingScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcB1JsonCodecScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcB2ProtobufCodecScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcB3MessagePackCodecScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcB4CodecCoexistenceScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.scenarios.RcB6JsonGoldenScenario
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ClientOptions
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ProcessSupport
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class ClientScenario(
    private val json: ObjectMapper,
    private val options: ClientOptions,
) {
    fun run() {
        val assert = ScenarioAssert()
        val server = ScenarioHttpClient(options.httpEndpoint, json)
        val codecRequester = ScenarioHttpClient(options.codecRequesterHttpEndpoint, json)

        val scenarios = linkedMapOf<String, () -> Unit>(
            "RC-A1" to { AutoRegistrationScenario(server, assert).run() },
            "RC-A2" to { AttributeRegistrationScenario(server, assert).run() },
            "RC-A3" to { ManualRegistrationScenario(server, assert).run() },
            "RC-A4" to { RcA4DiLifecycleScenario(server, assert).run() },
            "RC-A5" to { RcA5FilterOrderingScenario(server, assert).run() },
            "RC-B1" to { RcB1JsonCodecScenario(server, assert).run() },
            "RC-B2" to { RcB2ProtobufCodecScenario(server, assert).run() },
            "RC-B3" to { RcB3MessagePackCodecScenario(server, assert).run() },
            "RC-B4" to { RcB4CodecCoexistenceScenario(server, assert).run() },
            "RC-B6" to { RcB6JsonGoldenScenario(server, assert).run() },
            "RC-B5" to { CodecMismatchScenario(codecRequester, assert).run() },
        )
        when (options.mode) {
            "", "all" -> scenarios.filterKeys { it != "RC-B5" }.values.forEach { it() }
            ProcessSupport.CODEC_MISMATCH_MODE -> CodecMismatchScenario(codecRequester, assert).run()
            else -> {
                val run = scenarios[options.mode]
                    ?: throw IllegalArgumentException("Unknown RegistrationCodec scenario '${options.mode}'.")
                run()
            }
        }
    }
}
