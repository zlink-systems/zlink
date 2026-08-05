// 자립형 가이드 예제: PUB → SUB 발행/구독을 recv로 받는다.
//   bindings/java/gradlew -p . :kotlin-samples:runPubSubRecvSample --no-daemon
package systems.zlink.samples

import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.eventing.MonitorEventType
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.messaging.TopicMessage
import systems.zlink.contracts.sockets.RecvFlags

fun main() {
// --8<-- [start:doc]
    SampleSupport.ensureNative()
    val endpoint = SampleSupport.tcpEndpoint()
    val published = "${SampleSupport.PUBSUB_TOPIC}/${SampleSupport.PUBSUB_PAYLOAD}"

    Zlink.createContext().use { ctx ->
        ctx.createPubSocket().use { pub ->
            ctx.createSubSocket().use { sub ->
                pub.monitorOpen(MonitorEventType.CONNECTION_READY).use { pubMonitor ->
                    sub.monitorOpen(MonitorEventType.CONNECTION_READY).use { subMonitor ->
                        pub.bind(endpoint)
                        sub.setSubscription(SampleSupport.PUBSUB_TOPIC)
                        sub.connect(endpoint)
                        SampleSupport.waitPubSubReady(pubMonitor, subMonitor)

                        Message.from(SampleSupport.PUBSUB_PAYLOAD).use { payload ->
                            pub.publish(SampleSupport.PUBSUB_TOPIC).message(payload).submit()
                        }

                        TopicMessage().use { received ->
                            check(sub.subscribe(received, RecvFlags.NONE)) { "no pubsub delivery" }
                            val value = "${received.topic()}/${received.singlePartOrThrow().toUtf8String()}"
                            check(published == value) { "unexpected delivery: $value" }
                            println("[pubsub/recv] publish: \"$published\" → subscribe: \"$value\"")
                        }
                    }
                }
            }
        }
    }
// --8<-- [end:doc]
}
