package systems.zlink.e2e.kotlin.runtimemonitoring.trigger.transient

import org.springframework.boot.SpringBootConfiguration
import org.springframework.boot.autoconfigure.EnableAutoConfiguration
import org.springframework.context.annotation.Bean
import org.springframework.core.env.Environment
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootConfiguration(proxyBeanMethods = false)
@EnableAutoConfiguration
class TransientTriggerApplication {
    @Bean
    fun frameworkConfigurer(environment: Environment): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.addClientServerChannel(Contracts.CHANNEL)
                .client()
                .connect(environment.getRequiredProperty("zlink.e2e.transient-endpoint"))
        }
}
