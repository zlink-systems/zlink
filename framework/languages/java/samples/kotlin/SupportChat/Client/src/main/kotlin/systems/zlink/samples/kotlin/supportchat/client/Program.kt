package systems.zlink.samples.kotlin.supportchat.client

import java.net.URI
import java.time.Duration
import kotlinx.coroutines.future.await
import kotlinx.coroutines.runBlocking
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.stream.connector.ZLinkStreamConnector
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

fun main(args: Array<String>) = runBlocking {
    val options = ClientOptions.parse(args)
    val clients = List(6) { createClient(options) }
    try {
        SupportChatClientScenario().run(
            agent = clients[0],
            customer1 = clients[1],
            customer2 = clients[2],
            reconnectingAgent = clients[3],
            reconnectingCustomer = clients[4],
            waitingCustomer = clients[5],
        )
        println(SampleNames.ClientMarker)
    } finally {
        clients.forEach { client ->
            runCatching { client.close().submit().await() }
        }
    }
}

private fun createClient(options: ClientOptions): ZLinkStreamConnector =
    ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            options.streamEndpoint,
            ZLinkStreamDispatchMode.IMMEDIATE,
            SampleTimings.RequestTimeout,
            2,
            SampleTimings.ConnectTimeout,
            64 * 1024,
            true,
            Duration.ofSeconds(1),
            SampleTimings.RequestTimeout.plusSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
        ),
    )

private data class ClientOptions(val streamEndpoint: URI) {
    companion object {
        fun parse(args: Array<String>): ClientOptions {
            require(args.size == 2 && args[0] == "--stream-endpoint") {
                "Usage: Client --stream-endpoint <tcp://host:port>"
            }
            val endpoint = URI.create(args[1])
            require(endpoint.scheme == "tcp" && !endpoint.host.isNullOrBlank() && endpoint.port in 1..65535) {
                "--stream-endpoint must be a valid tcp endpoint"
            }
            return ClientOptions(endpoint)
        }
    }
}
