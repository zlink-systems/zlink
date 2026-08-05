package systems.zlink.samples.kotlin.bingo.client

import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.samples.kotlin.bingo.client.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.client.configuration.SampleTopology
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

suspend fun main(args: Array<String>) {
    val topology = SampleTopology.load(args)
    val client1 = createClient(topology.sessionAStreamEndpoint)
    val client2 = createClient(topology.sessionBStreamEndpoint)
    val observer = createClient(topology.sessionBStreamEndpoint)
    try {
        BingoClientScenario().run(client1, client2, observer)
    } finally {
        client1.close().await()
        client2.close().await()
        observer.close().await()
    }
    println("bingo=completed")
}

private fun createClient(endpoint: String): ZLinkKotlinStreamConnector {
    val client = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            SampleTimings.RequestTimeout,
            2,
            SampleTimings.ConnectTimeout,
            64 * 1024,
            false,
            Duration.ofSeconds(1),
            SampleTimings.RequestTimeout.plusSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            ZLinkProtobufCodec.defaultCodec(),
        ),
    )
    client.observeInbound { observation ->
        println(
            "stream-inbound sample=Bingo kind=${observation.kind()} " +
                "name=${observation.packetName()} bytes=${observation.payloadLength()}",
        )
        CompletableFuture.completedFuture(null)
    }
    return client.kotlin()
}
