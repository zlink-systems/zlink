package systems.zlink.e2e.kotlin.pubsub.publisher

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.beans.factory.ObjectProvider
import org.springframework.context.annotation.Bean
import org.springframework.context.ConfigurableApplicationContext
import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

private lateinit var parsedOptions: PublisherOptions

fun main(args: Array<String>) {
    parsedOptions = PublisherOptions.parse(args)
    val builder = SpringApplicationBuilder(PublisherApplication::class.java)
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    builder.run(*args)
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class PublisherApplication {
    @Bean
    fun publisherOptions(): PublisherOptions = parsedOptions

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun publisherEndpoints(
        options: PublisherOptions,
        fanout: ZLinkFanoutClient,
        json: ObjectMapper,
        application: ConfigurableApplicationContext,
        runtime: ObjectProvider<systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime>,
    ): PublisherEndpoints =
        PublisherEndpoints(fanout, json, options.httpEndpoint, options.channelName, application, runtime)

    @Bean
    fun publisherFramework(options: PublisherOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${parsedOptions.logDir}/publisher-flow.log")
                .traceLabel("kotlin-ps-publisher")
            val channel = options.addFanoutChannel(parsedOptions.channelName)
            parsedOptions.routingId?.let { channel.setRoutingId(RoutingId.from(it)) }
            parsedOptions.routingIdPrefix?.let { channel.setRoutingIdPrefix(it) }
            parsedOptions.advertiseHost?.let { channel.setAdvertiseHost(it) }
            val listenPort = parsedOptions.listenPort
            if (listenPort != null) {
                channel.enablePublisher(listenPort)
            } else {
                channel.enablePublisher(parsedOptions.publisherEndpoint)
            }
        }

    @Bean
    fun locationStore(options: PublisherOptions): ZLinkRedisLocationStore? =
        if (options.redisLocationEndpoint == null) {
            null
        } else {
            ZLinkRedisLocationStore(
                ZLinkRedisLocationOptions()
                    .setConnectionString(options.redisLocationEndpoint)
                    .setKeyPrefix(options.locationKeyPrefix!!),
            )
        }
}
