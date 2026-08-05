// 자립형 가이드 예제: PAIR 소켓으로 메시지를 보내고 recv로 받는다.
//   bindings/java/gradlew -p . :kotlin-samples:runPairRecvSample --no-daemon
package systems.zlink.samples

import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.eventing.MonitorEventType
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.messaging.Received
import systems.zlink.contracts.sockets.RecvFlags

fun main() {
// --8<-- [start:doc]
    SampleSupport.ensureNative()
    val endpoint = SampleSupport.tcpEndpoint()

    Zlink.createContext().use { ctx ->
        ctx.createPairSocket().use { server ->
            ctx.createPairSocket().use { client ->
                server.monitorOpen(MonitorEventType.CONNECTION_READY).use { serverMonitor ->
                    client.monitorOpen(MonitorEventType.CONNECTION_READY).use { clientMonitor ->
                        server.bind(endpoint)
                        client.connect(endpoint)
                        SampleSupport.waitConnected(serverMonitor, clientMonitor)

                        Message.from(SampleSupport.PAIR_PAYLOAD).use { outbound ->
                            client.send().message(outbound).submit()
                        }

                        Received().use { received ->
                            server.recv(received, RecvFlags.NONE)
                            val value = SampleSupport.singleUtf8(received)
                            check(SampleSupport.PAIR_PAYLOAD == value) { "unexpected payload: $value" }
                            println(
                                "[pair/recv] send: \"${SampleSupport.PAIR_PAYLOAD}\"" +
                                    " → recv: \"$value\""
                            )
                        }
                    }
                }
            }
        }
    }
// --8<-- [end:doc]
}
