package systems.zlink.samples.deliverydispatch.server.couriersession;

import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        CourierSessionApplication.run(SampleTopology.configPath(args));
    }
}
