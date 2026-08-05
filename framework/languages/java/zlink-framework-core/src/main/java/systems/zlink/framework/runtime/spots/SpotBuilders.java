package systems.zlink.framework.runtime.spots;

import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.configuration.ZLinkSpotMeshBuilder;
import systems.zlink.framework.runtime.internal.configuration.ZLinkSpotNodeBuilder;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.configuration.ZLinkActorFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;

public final class SpotBuilders {
    private SpotBuilders() {
    }

    public static Mesh mesh(
        String meshName,
        SpotNodeRegistration node,
        ZLinkFrameworkRegistration registration,
        Consumer<Class<?>> spotFactoryAdded) {
        return new Mesh(meshName, node, registration, spotFactoryAdded);
    }

    public record Mesh(
        String meshName,
        SpotNodeRegistration node,
        ZLinkFrameworkRegistration registration,
        Consumer<Class<?>> spotFactoryAdded) implements ZLinkSpotMeshBuilder {
        @Override
        public ZLinkSpotNodeBuilder setRoutingId(RoutingId routingId) {
            node.setRoutingId(routingId);
            return this;
        }

        public ZLinkSpotNodeBuilder enableRouter(String endpoint) {
            node.enableRouter();
            node.setRouterBind(endpoint);
            return this;
        }

        @Override
        public ZLinkSpotNodeBuilder connectRouter(String endpoint) {
            node.addRouterManualConnection(endpoint);
            return this;
        }

        @Override
        public ZLinkSpotNodeBuilder connectRouter(RoutingId peerRoutingId, String endpoint) {
            node.addRouterManualConnection(peerRoutingId, endpoint);
            return this;
        }

        @Override
        public ZLinkSpotNodeBuilder enablePubSub(String endpoint) {
            node.enablePubSub();
            node.setPubBind(endpoint);
            return this;
        }

        @Override
        public ZLinkSpotNodeBuilder connectPeerPub(String endpoint) {
            node.addPubSubManualConnection(endpoint);
            return this;
        }

        public ZLinkMeshObjectRoleBuilder objects() {
            return new ObjectRoles(node);
        }
    }

    private static final class ObjectRoles
        implements ZLinkMeshObjectRoleBuilder,
            ZLinkMeshObjectClientBuilder,
            ZLinkMeshObjectServerBuilder {
        private final SpotNodeRegistration node;

        private ObjectRoles(SpotNodeRegistration node) {
            this.node = node;
        }

        @Override
        public ZLinkMeshObjectClientBuilder client() {
            return this;
        }

        @Override
        public ZLinkMeshObjectServerBuilder server() {
            return this;
        }

        @Override
        public ZLinkMeshObjectServerBuilder addEntrySpot(
            Class<? extends ZLinkEntrySpot<?>> entrySpotType) {
            node.registerEntrySpot(entrySpotType);
            return this;
        }

        @Override
        public <TSpot extends ZLinkSpot<?>> ZLinkMeshObjectServerBuilder addSpotFactory(
            String stableType,
            Class<TSpot> spotType,
            Consumer<ZLinkUserSpotFactoryBuilder<TSpot>> configure) {
            LegacyUserSpotBuilder<TSpot> builder = new LegacyUserSpotBuilder<>();
            builder.configure(configure);
            node.registerSpotFactory(spotType);
            return this;
        }

        @Override
        public <TSpot extends ZLinkInstanceSpot>
        ZLinkMeshObjectServerBuilder addInstanceSpotFactory(
            String stableType,
            Class<TSpot> spotType,
            Consumer<ZLinkInstanceSpotFactoryBuilder<TSpot>> configure) {
            throw new ZLinkConfigurationException(
                "legacy Spot topology does not support Instance Spot factories");
        }

        @Override
        public <TActor extends ZLinkActor> ZLinkMeshObjectServerBuilder addActorFactory(
            String stableType,
            Class<TActor> actorType,
            Class<? extends ZLinkActorFactory> factoryType,
            Consumer<ZLinkActorFactoryBuilder<TActor>> configure) {
            LegacyActorBuilder<TActor> builder = new LegacyActorBuilder<>();
            builder.configure(configure);
            node.registerActorFactory(stableType, factoryType);
            return this;
        }
    }

    private abstract static class LegacyRelocationBuilder {
        private boolean selected;
        private boolean accepting = true;

        final <T> void configure(Consumer<T> configure) {
            try {
                @SuppressWarnings("unchecked")
                T self = (T) this;
                configure.accept(self);
                if (!selected) {
                    throw new ZLinkConfigurationException(
                        "factory relocation behavior must be selected exactly once");
                }
            } finally {
                accepting = false;
            }
        }

        final void select() {
            if (!accepting || selected) {
                throw new ZLinkConfigurationException(
                    "factory relocation behavior must be selected exactly once");
            }
            selected = true;
        }
    }

    private static final class LegacyActorBuilder<TActor extends ZLinkActor>
        extends LegacyRelocationBuilder
        implements ZLinkActorFactoryBuilder<TActor> {
        @Override public void disableRelocation() { select(); }
        @Override public void recreateOnRelocation() { select(); }
        @Override public void preserveStateWith(
            Class<? extends ZLinkActorRelocationAdapter<TActor>> adapterClass) {
            if (adapterClass == null) {
                throw new ZLinkConfigurationException(
                    "relocation adapterClass is required");
            }
            select();
        }
    }

    private static final class LegacyUserSpotBuilder<TSpot extends ZLinkSpot<?>>
        extends LegacyRelocationBuilder
        implements ZLinkUserSpotFactoryBuilder<TSpot> {
        @Override
        public ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int limit) {
            if (limit <= 0) {
                throw new ZLinkConfigurationException(
                    "stableTypeLimit must be positive");
            }
            return this;
        }
        @Override
        public ZLinkUserSpotFactoryBuilder<TSpot> executionMode(
            ZLinkUserSpotExecutionMode mode) {
            return this;
        }
        @Override
        public ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(
            ZLinkSpotRelocationReadinessMode mode) {
            return this;
        }
        @Override public void disableRelocation() { select(); }
        @Override public void recreateOnRelocation() { select(); }
        @Override public void preserveStateWith(
            Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass) {
            if (adapterClass == null) {
                throw new ZLinkConfigurationException(
                    "relocation adapterClass is required");
            }
            select();
        }
    }
}
