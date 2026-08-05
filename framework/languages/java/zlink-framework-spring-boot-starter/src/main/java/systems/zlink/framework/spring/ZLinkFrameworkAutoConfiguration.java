package systems.zlink.framework.spring;

import java.util.List;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.beans.factory.config.BeanFactoryPostProcessor;
import org.springframework.beans.factory.config.AutowireCapableBeanFactory;
import org.springframework.boot.autoconfigure.AutoConfiguration;
import org.springframework.boot.autoconfigure.condition.ConditionalOnBean;
import org.springframework.boot.autoconfigure.condition.ConditionalOnMissingBean;
import org.springframework.boot.autoconfigure.condition.ConditionalOnClass;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Lazy;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import systems.zlink.httpclient.ZLinkFrameworkHttpExecutionTurn;
import systems.zlink.httpclient.ZLinkHttpExecutionTurn;

@AutoConfiguration
public class ZLinkFrameworkAutoConfiguration {
    @Bean
    @ConditionalOnMissingBean
    public ZLinkHttpExecutionTurn zlinkHttpExecutionTurn() {
        return new ZLinkFrameworkHttpExecutionTurn();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkEnabled.class)
    public static BeanFactoryPostProcessor zlinkFrameworkCapabilityBeanRegistrar() {
        return new ZLinkFrameworkCapabilityBeanRegistrar();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkEnabled.class)
    @ConditionalOnMissingBean
    public DefaultZLinkFrameworkOptions zlinkFrameworkOptions(
        List<ZLinkFrameworkConfigurer> configurers) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        for (ZLinkFrameworkConfigurer configurer : configurers) {
            configurer.configure(options);
        }
        return options;
    }

    @Bean
    @ConditionalOnMissingBean
    public ZLinkBackendAdapterProvider zlinkBackendAdapterFactory() {
        return new ZLinkJavaBackendAdapterFactory();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkEnabled.class)
    public ZLinkFrameworkConfigurer zlinkLocationStoreConfigurer(
        ObjectProvider<ZLinkLocationStore> locationStore) {
        return options -> {
            ZLinkLocationStore unified = locationStore.getIfUnique();
            if (unified != null) {
                options.addLocationStore(unified);
            }
        };
    }

    @Bean
    @ConditionalOnBean(DefaultZLinkFrameworkOptions.class)
    @ConditionalOnMissingBean
    public ZLinkHandlerActivator zlinkHandlerFactory(AutowireCapableBeanFactory beanFactory) {
        return new ZLinkSpringHandlerFactory(beanFactory);
    }

    @Bean
    @ConditionalOnBean(DefaultZLinkFrameworkOptions.class)
    @ConditionalOnMissingBean
    public ZLinkFrameworkLifecycle zlinkFrameworkLifecycle(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendAdapterFactory,
        ZLinkHandlerActivator handlerFactory) {
        return new ZLinkFrameworkLifecycle(
            options,
            backendAdapterFactory,
            handlerFactory);
    }

    @Bean
    @Lazy
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
        zlinkFrameworkRuntime(ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle.runtimeBean();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public ZLinkClient zlinkClient(ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle;
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public ZLinkChannelRuntimeOptions zlinkChannelRuntimeOptions(
        ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle;
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public ZLinkFanoutClient zlinkFanoutClient(ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle;
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public ZLinkRouteClient zlinkRouteClient(ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle;
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public ZLinkRouteMeshRuntime zlinkRouteMeshRuntime(
        ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle.routeMeshRuntime();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public systems.zlink.framework.monitoring.ZLinkClientServerRuntime
        zlinkClientServerRuntime(ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle.clientServerRuntime();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public systems.zlink.framework.monitoring.ZLinkFanoutRuntime
        zlinkFanoutRuntime(ZLinkFrameworkLifecycle lifecycle) {
        return lifecycle.fanoutRuntime();
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public ZLinkRouteMeshRuntimeOptions zlinkRouteMeshRuntimeOptions(
        ZLinkFrameworkLifecycle lifecycle) {
        return new systems.zlink.framework.spring.internal.runtime.ZLinkRouteMeshRuntimeOptionsService(
            lifecycle);
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public systems.zlink.framework.spots.SpotHandleResolver zlinkSpotHandleResolver(
        ZLinkFrameworkLifecycle lifecycle) {
        return new systems.zlink.framework.spots.SpotHandleResolver() {
            @Override
            public java.util.concurrent.CompletionStage<java.util.Optional<
                systems.zlink.framework.spots.SpotHandle>> resolveSpotHandle(
                    String meshName,
                    String spotId) {
                return lifecycle.spotHandleResolver().resolveSpotHandle(meshName, spotId);
            }

            @Override
            public java.util.concurrent.CompletionStage<java.util.Optional<
                systems.zlink.framework.spots.SpotHandle>> resolveSpotHandle(
                    String spotId) {
                return lifecycle.spotHandleResolver().resolveSpotHandle(spotId);
            }
        };
    }

    @Bean
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean
    public systems.zlink.framework.spots.ActorSpotHandleResolver zlinkActorSpotHandleResolver(
        ZLinkFrameworkLifecycle lifecycle) {
        return actorId -> lifecycle.actorSpotHandleResolver().resolveActorSpotHandle(actorId);
    }

    @Bean("zlinkDrainReadiness")
    @ConditionalOnClass(name = "org.springframework.boot.actuate.health.HealthIndicator")
    @ConditionalOnBean(ZLinkFrameworkLifecycle.class)
    @ConditionalOnMissingBean(name = "zlinkDrainReadiness")
    public ZLinkDrainReadinessContributor zlinkDrainReadinessContributor(
        ZLinkFrameworkLifecycle lifecycle) {
        return new ZLinkDrainReadinessContributor(lifecycle);
    }

}
