package systems.zlink.samples.deliverydispatch.server.courierspotnode;

import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        CourierSpotNodeApplication.run(SampleTopology.configPath(args));
    }
}
