package systems.zlink.framework.configuration;

import java.time.Duration;
import java.util.concurrent.Executor;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkRelocationStore;

public interface ZLinkFrameworkOptions {
    Duration defaultRequestTimeout();

    void setDefaultRequestTimeout(Duration timeout);

    ZLinkCodecRegistryBuilder codecs();

    void addHandlersFromPackageOf(Class<?> markerType);

    ZLinkMetadataPolicyBuilder configureMetadata();

    ZLinkMeshNodeBuilder addRouteMesh(String meshName);

    ClientServerChannelBuilder addClientServerChannel(String channelName);

    FanoutChannelBuilder addFanoutChannel(String channelName);

    ZLinkStreamNodeBuilder addStreamNode(String streamNodeName);

    void addLocationStore(ZLinkLocationStore store);

    void addRelocationStore(ZLinkRelocationStore store);

    void setApplicationVersion(long version);

    void setMaintenanceWave(String waveId);

    ZLinkLocationOptions configureLocations();

    ZLinkInboundDispatchOptions configureInboundDispatch();

    ZLinkNetworkOptions configureNetwork();

    void useFilter(Class<? extends ZLinkHandlerFilter> filterType);

    ZLinkDispatchOptions configureDispatch();

    ZLinkStreamCompressionBuilder configureStreamCompression();

    ZLinkWorkerOptions configureWorkers();

    void useVirtualThreadHandlers();

    void useHandlerExecutor(Executor executor);
}
