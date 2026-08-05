package systems.zlink.e2e.observabilityops.verifier;

import java.nio.file.Path;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 1 || args.length > 2) {
            throw new IllegalArgumentException(
                "Usage: observability-ops-verifier <evidence-directory> [all|OBS-A1..OBS-C5]");
        }
        String selector = args.length == 2 ? args[1] : "all";
        for (String scenario : new EvidenceVerifier().verify(Path.of(args[0]), selector)) {
            System.out.println(scenario + " PASS");
        }
        System.out.println("observability-ops result=passed");
    }
}
