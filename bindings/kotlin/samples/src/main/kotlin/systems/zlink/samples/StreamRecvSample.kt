// 자립형 가이드 예제: STREAM 소켓으로 raw TCP 페이로드를 recv하고 에코한다.
//   bindings/java/gradlew -p . :kotlin-samples:runStreamRecvSample --no-daemon
package systems.zlink.samples

import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.eventing.MonitorEventType
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.messaging.Received
import systems.zlink.contracts.sockets.RecvFlags
import java.nio.charset.StandardCharsets

fun main() {
// --8<-- [start:doc]
    SampleSupport.ensureNative()
    val endpoint = SampleSupport.tcpEndpoint()

    Zlink.createContext().use { ctx ->
        ctx.createStreamSocket().use { server ->
            server.monitorOpen(
                MonitorEventType.ACCEPTED,
                MonitorEventType.CONNECTION_READY
            ).use { monitor ->
                server.bind(endpoint)
                SampleSupport.connectRawTcp(endpoint).use { rawClient ->
                    SampleSupport.waitStreamConnected(monitor)

                    val payload = SampleSupport.STREAM_PAYLOAD.toByteArray(StandardCharsets.UTF_8)
                    SampleSupport.sendRawTcp(rawClient, payload)

                    Received().use { received ->
                        server.recv(received, RecvFlags.NONE)
                        val value = SampleSupport.singleUtf8(received)
                        check(SampleSupport.STREAM_PAYLOAD == value && received.routingId.isPresent) {
                            "unexpected stream delivery"
                        }
                        Message.from(SampleSupport.STREAM_PAYLOAD).use { reply ->
                            received.send().message(reply).submit()
                        }
                    }

                    val echoed = String(
                        SampleSupport.recvExactRawTcp(rawClient, payload.size),
                        StandardCharsets.UTF_8
                    )
                    check(SampleSupport.STREAM_PAYLOAD == echoed) { "unexpected stream echo: $echoed" }
                    println(
                        "[stream/recv] send: \"${SampleSupport.STREAM_PAYLOAD}\"" +
                            " → recv: \"$echoed\""
                    )
                }
            }
        }
    }
// --8<-- [end:doc]
}
