// 자립형 가이드 예제: 소켓 모니터로 connection-ready 이벤트를 recv한다.
//   bindings/java/gradlew -p . :kotlin-samples:runMonitorRecvSample --no-daemon
package systems.zlink.samples

import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.eventing.MonitorEventType

fun main() {
    SampleSupport.ensureNative()
    val endpoint = SampleSupport.tcpEndpoint()

    Zlink.createContext().use { ctx ->
        ctx.createPairSocket().use { server ->
            ctx.createPairSocket().use { client ->
                server.monitorOpen(MonitorEventType.CONNECTION_READY).use { serverMonitor ->
                    client.monitorOpen(MonitorEventType.CONNECTION_READY).use { clientMonitor ->
                        server.bind(endpoint)
                        client.connect(endpoint)

                        val serverEvent = serverMonitor.recv()
                        val clientEvent = clientMonitor.recv()
                        check(
                            serverEvent.event() == MonitorEventType.CONNECTION_READY &&
                                clientEvent.event() == MonitorEventType.CONNECTION_READY
                        ) { "expected connection-ready events" }
                        println("[monitor/recv] recv: \"connection-ready\"")
                    }
                }
            }
        }
    }
}
