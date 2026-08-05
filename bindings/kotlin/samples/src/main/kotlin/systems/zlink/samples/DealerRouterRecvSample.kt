// 자립형 가이드 예제: DEALER → ROUTER 요청/응답을 recv로 받는다.
//   bindings/java/gradlew -p . :kotlin-samples:runDealerRouterRecvSample --no-daemon
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
        ctx.createRouterSocket().use { router ->
            ctx.createDealerSocket().use { dealer ->
                router.monitorOpen(MonitorEventType.CONNECTION_READY).use { routerMonitor ->
                    dealer.monitorOpen(MonitorEventType.CONNECTION_READY).use { dealerMonitor ->
                        router.bind(endpoint)
                        dealer.connect(endpoint)
                        SampleSupport.waitConnected(routerMonitor, dealerMonitor)

                        Message.from(SampleSupport.DEALER_REQUEST).use { request ->
                            dealer.send().message(request).submit()
                        }

                        Received().use { received ->
                            router.recv(received, RecvFlags.NONE)
                            val value = SampleSupport.singleUtf8(received)
                            check(SampleSupport.DEALER_REQUEST == value) { "unexpected request: $value" }
                            Message.from(SampleSupport.DEALER_REPLY).use { reply ->
                                received.send().message(reply).submit()
                            }
                        }

                        Received().use { received ->
                            dealer.recv(received, RecvFlags.NONE)
                            val value = SampleSupport.singleUtf8(received)
                            check(SampleSupport.DEALER_REPLY == value) { "unexpected reply: $value" }
                            println(
                                "[dealer-router/recv] send: \"${SampleSupport.DEALER_REQUEST}\"" +
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
