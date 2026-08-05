package systems.zlink.framework.runtime.channels;

import java.util.Map;
import java.util.Objects;
import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshNodeRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

/**
 * Applies live RouteMesh options to the Framework-owned MeshNode runtime.
 */
public final class ZLinkRouteMeshRuntimeOptionsRuntime
    implements ZLinkRouteMeshRuntimeOptions {
    private final Map<String, ZLinkInternalMeshNode> nodes;
    private final Runnable descriptorChanged;

    public ZLinkRouteMeshRuntimeOptionsRuntime(
        Map<String, ZLinkInternalMeshNode> nodes,
        Runnable descriptorChanged) {
        this.nodes = Map.copyOf(Objects.requireNonNull(nodes, "nodes"));
        this.descriptorChanged =
            Objects.requireNonNull(descriptorChanged, "descriptorChanged");
    }

    @Override
    public ZLinkMeshNodeRuntimeOptions meshNode(String meshName) {
        ZLinkInternalMeshNode node = requireNode(meshName);
        return new ZLinkMeshNodeRuntimeOptions() {
            @Override
            public long maxMessageSize() {
                return node.maxMessageSize();
            }

            @Override
            public void maxMessageSize(long value) {
                if (value < 0) {
                    throw new ZLinkConfigurationException(
                        "maxMessageSize must not be negative");
                }
                node.setMaxMessageSize(value);
            }
        };
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(
        String meshName,
        String channelName) {
        return channel(requireNode(meshName), channelName);
    }

    @Override
    public ZLinkMeshPlacementRuntimeOptions mesh(String meshName) {
        ZLinkInternalMeshNode node = requireNode(meshName);
        return new ZLinkMeshPlacementRuntimeOptions() {
            @Override
            public int placementWeight() {
                return node.placementWeight();
            }

            @Override
            public void setPlacementWeight(int value) {
                validateWeight(value);
                if (node.placementWeight() == value) {
                    return;
                }
                node.setPlacementWeight(value);
                descriptorChanged.run();
            }
        };
    }

    @Override
    public ZLinkMeshChannelRuntimeOptions channel(String channelName) {
        requireText(channelName, "channelName");
        ZLinkInternalMeshNode found = null;
        for (ZLinkInternalMeshNode node : nodes.values()) {
            if (!node.channelWeights().containsKey(channelName)) {
                continue;
            }
            if (found != null) {
                throw new ZLinkConfigurationException(
                    "ChannelName is registered by more than one RouteMesh: "
                        + channelName);
            }
            found = node;
        }
        if (found == null) {
            throw new ZLinkConfigurationException(
                "RouteMesh channel is not registered: " + channelName);
        }
        return channel(found, channelName);
    }

    private ZLinkMeshChannelRuntimeOptions channel(
        ZLinkInternalMeshNode node,
        String channelName) {
        requireText(channelName, "channelName");
        if (!node.channelWeights().containsKey(channelName)) {
            throw new ZLinkConfigurationException(
                "RouteMesh channel is not registered"
                    + " [mesh=" + node.name()
                    + ", channel=" + channelName + "]");
        }
        return new ZLinkMeshChannelRuntimeOptions() {
            @Override
            public int weight() {
                return node.channelWeights().get(channelName);
            }

            @Override
            public void weight(int value) {
                validateWeight(value);
                if (weight() == value) {
                    return;
                }
                node.setChannelWeight(channelName, value);
                descriptorChanged.run();
            }
        };
    }

    private ZLinkInternalMeshNode requireNode(String meshName) {
        requireText(meshName, "meshName");
        ZLinkInternalMeshNode node = nodes.get(meshName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                "RouteMesh is not registered: " + meshName);
        }
        return node;
    }

    private static void validateWeight(int value) {
        if (value < 0 || value > 10_000) {
            throw new ZLinkConfigurationException(
                "weight must be in range 0..10000");
        }
    }

    private static void requireText(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new ZLinkConfigurationException(name + " is required");
        }
    }
}
