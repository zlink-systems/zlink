package systems.zlink.framework.spring.internal.runtime;

import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;

final class ZLinkRouteMeshRuntimeOptionsServiceTest {
    @Test
    void runtimeOptionsDelegatesToCoreRuntimeOptions() {
        var meshChannelOptions =
                new ZLinkMeshChannelRuntimeOptions() {
                    @Override
                    public int weight() {
                        return 100;
                    }

                    @Override
                    public void weight(int value) {}
                };
        var placementOptions =
                new ZLinkMeshPlacementRuntimeOptions() {
                    @Override
                    public int placementWeight() {
                        return 100;
                    }

                    @Override
                    public void setPlacementWeight(int value) {}
                };
        ZLinkRouteMeshRuntimeOptions delegate =
                new ZLinkRouteMeshRuntimeOptions() {
                    @Override
                    public ZLinkMeshChannelRuntimeOptions channel(
                            String meshName, String channelName) {
                        return meshChannelOptions;
                    }

                    @Override
                    public ZLinkMeshPlacementRuntimeOptions mesh(String meshName) {
                        return placementOptions;
                    }

                    @Override
                    public ZLinkMeshChannelRuntimeOptions channel(String channelName) {
                        return meshChannelOptions;
                    }
                };
        var options = new ZLinkRouteMeshRuntimeOptionsService(() -> delegate);

        assertSame(meshChannelOptions, options.channel("mesh", "channel"));
        assertSame(placementOptions, options.mesh("mesh"));
        assertSame(meshChannelOptions, options.channel("channel"));
    }
}
