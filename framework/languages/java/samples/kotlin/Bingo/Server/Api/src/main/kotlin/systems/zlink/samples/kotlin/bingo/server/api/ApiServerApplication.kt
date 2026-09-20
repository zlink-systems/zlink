package systems.zlink.samples.kotlin.bingo.server.api

import java.net.URI
import java.nio.file.Path
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.bingo.server.configuration.BingoReadinessReporter
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTopology

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [ApiServerApplication::class],
)
class ApiServerApplication {
    @Bean
    fun apiFramework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.addHandlersFromPackageOf(ApiServerApplication::class.java)
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch { messageFlow(ZLinkMessageFlowLogMode.NORMAL) }
            // --8<-- [start:doc-codec-register]
            // Every payload this process sends is encoded with Protobuf instead of the default
            // codec.
            options.codecs().use(ZLinkProtobufCodec.defaultCodec())
            // --8<-- [end:doc-codec-register]
            options.configureLocations()
            val api =
                options
                    .addRouteMesh(SampleNames.Mesh)
                    .setRoutingIdPrefix("api")
                    .listen(topology.selectedApiMeshEndpoint())
            api.objects().client()
            val apiChannelEndpoint = URI.create(topology.selectedApiChannelEndpoint())
            options
                .addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiChannelEndpoint.host)
                .listen(apiChannelEndpoint.port)
                .addHandlerGroup(SampleNames.ApiChannel)
            options
                .addRouteMesh(SampleNames.MatchmakingMesh)
                .setRoutingIdPrefix("api-matchmaking")
                .listen(topology.apiMatchmakingRouterEndpoint)
                .objects()
                .client()
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore =
        SampleLocationStore.create(topology)

    @Bean(destroyMethod = "close")
    fun bingoReadinessReporter(
        topology: SampleTopology,
        meshes: ZLinkRouteMeshRuntime,
    ): BingoReadinessReporter = BingoReadinessReporter.api(topology, meshes)

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val environment =
                StandardEnvironment().apply {
                    propertySources.remove(
                        StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME
                    )
                    propertySources.remove(
                        StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME
                    )
                }
            val configPath = SampleTopology.configPath(args)
            val builder =
                SpringApplicationBuilder(ApiServerApplication::class.java)
                    .environment(environment)
                    .properties(
                        "spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}"
                    )
                    .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run()
            return AutoCloseable { context.close() }
        }
    }
}
