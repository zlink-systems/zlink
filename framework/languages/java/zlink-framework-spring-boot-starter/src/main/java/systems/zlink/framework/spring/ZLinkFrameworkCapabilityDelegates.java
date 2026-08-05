package systems.zlink.framework.spring;

import org.springframework.beans.factory.config.ConfigurableListableBeanFactory;
import org.springframework.beans.factory.support.BeanDefinitionRegistry;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

final class ZLinkFrameworkCapabilityDelegates {
    private static final String SPOT_MANAGER_BEAN_NAME = "zlinkSpotManager";
    private static final String SPOT_OUTBOUND_BEAN_NAME = "zlinkSpotOutbound";
    private static final String SPOT_PUBLISHER_CLIENT_BEAN_NAME =
        "zlinkSpotPublisherClient";
    private static final String ACTOR_MANAGER_BEAN_NAME = "zlinkActorManager";
    private static final String ACTOR_DIRECTORY_BEAN_NAME = "zlinkActorDirectory";
    private static final String ACTOR_CLIENT_BEAN_NAME = "zlinkActorClient";

    private ZLinkFrameworkCapabilityDelegates() {
    }

    static void register(
        BeanDefinitionRegistry registry,
        ConfigurableListableBeanFactory beanFactory,
        DefaultZLinkFrameworkOptions options) {
        boolean hasSpotRuntime = !options.registration().spotNodes().isEmpty()
            || options.registration().meshNodes().stream().anyMatch(node ->
                node.objectRoleEnabled()
                    || !node.spotFactories().isEmpty()
                    || !node.entrySpots().isEmpty()
                    || !node.actorFactories().isEmpty());
        boolean hasActorFactory = options.registration().spotNodes().stream()
            .anyMatch(node -> !node.actorFactories().isEmpty())
            || options.registration().meshNodes().stream()
                .anyMatch(node -> !node.actorFactories().isEmpty());
        boolean hasObjectRole = options.registration().meshNodes().stream()
            .anyMatch(node -> node.objectRoleEnabled());
        boolean hasSpotPublisherClient = options.registration().spotNodes().stream()
            .anyMatch(node -> node.pubSubEnabled())
            || options.registration().meshNodes().stream()
                .anyMatch(node -> !node.channelNames().isEmpty());
        boolean hasLocationStore = options.registration().locations().enabled();

        if (hasSpotRuntime && !ZLinkSpringBeanDefinitions.hasBean(beanFactory, ZLinkSpotManager.class)) {
            ZLinkSpringBeanDefinitions.registerDelegate(
                registry,
                SPOT_MANAGER_BEAN_NAME,
                ZLinkFrameworkSpotManagerBean.class);
        }
        if (hasSpotRuntime && !ZLinkSpringBeanDefinitions.hasBean(beanFactory, ZLinkSpotOutbound.class)) {
            ZLinkSpringBeanDefinitions.registerDelegate(
                registry,
                SPOT_OUTBOUND_BEAN_NAME,
                ZLinkFrameworkSpotOutboundBean.class);
        }
        if (hasSpotPublisherClient
            && !ZLinkSpringBeanDefinitions.hasBean(beanFactory, ZLinkSpotPublisherClient.class)) {
            ZLinkSpringBeanDefinitions.registerDelegate(
                registry,
                SPOT_PUBLISHER_CLIENT_BEAN_NAME,
                ZLinkFrameworkSpotPublisherClientBean.class);
        }
        if (hasSpotRuntime
            && (hasActorFactory || hasObjectRole)
            && !ZLinkSpringBeanDefinitions.hasBean(beanFactory, ZLinkActorManager.class)) {
            ZLinkSpringBeanDefinitions.registerDelegate(
                registry,
                ACTOR_MANAGER_BEAN_NAME,
                ZLinkFrameworkActorManagerBean.class);
        }
        if (hasSpotRuntime
            && (hasActorFactory || hasLocationStore)
            && !ZLinkSpringBeanDefinitions.hasBean(beanFactory, ZLinkActorDirectory.class)) {
            ZLinkSpringBeanDefinitions.registerDelegate(
                registry,
                ACTOR_DIRECTORY_BEAN_NAME,
                ZLinkFrameworkActorDirectoryBean.class);
        }
        if (hasSpotRuntime
            && hasLocationStore
            && !ZLinkSpringBeanDefinitions.hasBean(beanFactory, ZLinkActorClient.class)) {
            ZLinkSpringBeanDefinitions.registerDelegate(
                registry,
                ACTOR_CLIENT_BEAN_NAME,
                ZLinkFrameworkActorClientBean.class);
        }
    }
}
