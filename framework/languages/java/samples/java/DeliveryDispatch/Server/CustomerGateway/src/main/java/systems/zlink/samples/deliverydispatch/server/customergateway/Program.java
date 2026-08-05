package systems.zlink.samples.deliverydispatch.server.customergateway;

import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        CustomerGatewayApplication.run(SampleTopology.configPath(args));
    }
}
