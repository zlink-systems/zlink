package systems.zlink.samples.deliverydispatch.server.dispatch;

import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        DispatchServerApplication.run(SampleTopology.configPath(args));
    }
}
