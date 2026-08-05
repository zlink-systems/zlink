package systems.zlink.e2e.kotlin.pubsub.subscriber

import com.fasterxml.jackson.databind.ObjectMapper
import java.util.concurrent.CompletableFuture
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

private lateinit var parsedOptions: SubscriberOptions

fun main(args: Array<String>) {
    parsedOptions = SubscriberOptions.parse(args)
    val builder = SpringApplicationBuilder(SubscriberApplication::class.java)
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    builder.run(*args)
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class SubscriberApplication {
    @Bean
    fun subscriberOptions(): SubscriberOptions = parsedOptions

    @Bean
    fun scenarioState(options: SubscriberOptions): EvidenceStore =
        EvidenceStore(
            options.rid,
            options.topics,
            options.handlerDelayMillis,
        )

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun evidenceHttpServer(
        options: SubscriberOptions,
        state: EvidenceStore,
        json: ObjectMapper,
    ): OperationalEndpoints =
        OperationalEndpoints(state, json, options.httpEndpoint)

    @Bean
    fun subscriberFramework(
        options: SubscriberOptions,
        state: EvidenceStore,
    ): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${parsedOptions.logDir}/${state.subscriberRid}-flow.log")
                .traceLabel("kotlin-ps-${state.subscriberRid}")
                .setMessageFlowObserver { error ->
                    if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
                        return@setMessageFlowObserver CompletableFuture.completedFuture(null)
                    }
                    state.record(
                        "DispatchError",
                        error.topic(),
                        "observer",
                        -1,
                        "${error.errorReason()}/${error.errorAction()}/${error.packetName()}",
                    )
                    CompletableFuture.completedFuture(null)
                }
            options.addHandlersFromPackageOf(EventMsgHandler::class.java)
            options.addFanoutChannel(Contracts.EVENT_CHANNEL)
                .enableSubscriber()
                .addHandlerGroup(Contracts.HANDLER_GROUP)
        }

    @Bean
    fun eventMsgHandler(state: EvidenceStore): EventMsgHandler =
        EventMsgHandler(state)

    @Bean
    fun locationStore(options: SubscriberOptions): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(options.redisLocationEndpoint)
                .setKeyPrefix(options.locationKeyPrefix),
        )
}
