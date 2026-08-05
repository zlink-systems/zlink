package systems.zlink.e2e.spotservice.client.Scenarios;

public final class ScenarioSuite {
    private ScenarioSuite() {
    }

    public static void run(String mode, SpotServiceScenarioContext context) {
        switch (mode) {
            case "SM-A1" -> SmA1Scenario.run(context);
            case "SM-A2" -> SmA2Scenario.run(context);
            case "SM-A3" -> SmA3Scenario.run(context);
            case "SM-A4" -> SmA4Scenario.run(context);
            case "SM-A5" -> SmA5Scenario.run(context);
            case "SM-A6" -> SmA6Scenario.run(context);
            case "SM-A7" -> SmA7Scenario.run(context);
            case "SM-A8" -> SmA8Scenario.run(context);
            case "SM-B1" -> SmB1Scenario.run(context);
            case "SM-B2" -> SmB2Scenario.run(context);
            case "SM-B3" -> SmB3Scenario.run(context);
            case "SM-B4" -> SmB4Scenario.run(context);
            case "SM-B5" -> SmB5Scenario.run(context);
            case "SM-B6" -> SmB6Scenario.run(context);
            case "SM-B7" -> SmB7Scenario.run(context);
            case "SM-B8" -> SmB8Scenario.run(context);
            case "SM-B9" -> SmB9Scenario.run(context);
            case "SM-C1" -> SmC1Scenario.run(context);
            case "SM-C2" -> SmC2Scenario.run(context);
            case "SM-C3" -> SmC3Scenario.run(context);
            case "SM-C4" -> SmC4Scenario.run(context);
            case "SM-C5" -> SmC5Scenario.run(context);
            case "SM-D1" -> SmD1Scenario.run(context);
            case "SM-D2" -> SmD2Scenario.run(context);
            case "SM-D3" -> SmD3Scenario.run(context);
            case "SM-D4" -> SmD4Scenario.run(context);
            case "SM-D5" -> SmD5Scenario.run(context);
            case "SM-D6" -> SmD6Scenario.run(context);
            case "SM-D7" -> SmD7Scenario.run(context);
            case "SM-D8" -> SmD8Scenario.run(context);
            case "SM-D9" -> SmD9Scenario.run(context);
            case "SM-D10" -> SmD10Scenario.run(context);
            case "SM-D11" -> SmD11Scenario.run(context);
            case "SM-D12" -> SmD12Scenario.run(context);
            case "SM-D13" -> SmD13Scenario.run(context);
            case "SM-D14" -> SmD14Scenario.run(context);
            case "SM-D15" -> SmD15Scenario.run(context);
            case "SM-E1" -> SmE1Scenario.run(context);
            case "SM-E2" -> SmE2Scenario.run(context);
            case "SM-E3" -> SmE3Scenario.run(context);
            case "SM-E4" -> SmE4Scenario.run(context);
            case "SM-F1" -> SmF1Scenario.run(context);
            case "SM-F2" -> SmF2Scenario.run(context);
            case "SM-F3" -> SmF3Scenario.run(context);
            case "SM-F4" -> SmF4Scenario.run(context);
            case "SM-F5" -> SmF5Scenario.run(context);
            case "SM-F6" -> SmF6Scenario.run(context);
            case "SM-G1" -> SmG1Scenario.run(context);
            case "SM-G2" -> SmG2Scenario.run(context);
            case "SM-G3" -> SmG3Scenario.run(context);
            case "SM-G4" -> SmG4Scenario.run(context);
            case "state1" -> SmA1Scenario.run(context);
            case "state2" -> SmA2Scenario.run(context);
            case "send" -> SendScenario.run(context);
            case "timeout" -> TimeoutScenario.run(context);
            case "missing" -> SmE1Scenario.run(context);
            case "normal" -> SmC1Scenario.run(context);
            case "owner" -> SmA3Scenario.run(context);
            case "stage-wrapper" -> SmA5Scenario.run(context);
            case "owner-remap" -> SmG2Scenario.run(context);
            case "route-mesh" -> SmF1Scenario.run(context);
            case "route-lifecycle" -> SmF5Scenario.run(context);
            case "actor-session" -> SmB1Scenario.run(context);
            case "remote-actor-session" -> SmB2Scenario.run(context);
            case "actor-missing" -> SmB5Scenario.run(context);
            case "actor-destroy" -> SmB8Scenario.run(context);
            case "actor-join-admission" -> SmB9Scenario.run(context);
            case "actor-push-chain" -> SmD15Scenario.run(context);
            case "actor-leave-disconnect" -> SmB6Scenario.run(context);
            case "actor-disconnect-notify" -> SmD5Scenario.run(context);
            case "bound-push-isolation" -> SmD6Scenario.run(context);
            case "stream-auth" -> SmD7Scenario.run(context);
            case "stream-reconnect" -> SmD8Scenario.run(context);
            case "stream-rebind-transfer" -> SmD12Scenario.run(context);
            case "multi-actor-bind" -> SmD4Scenario.run(context);
            case "stream-backpressure" -> SmD10Scenario.run(context);
            case "mixed-stream-channel" -> SmD11Scenario.run(context);
            case "stream-heartbeat" -> SmD13Scenario.run(context);
            case "stream-tls" -> SmD14Scenario.run(context);
            case "multi-node" -> SmQ9Scenario.run(context);
            case "spot-only-mesh" -> SmF6Scenario.run(context);
            case "bound-push-load" -> SmG4Scenario.run(context);
            case "join-leave-race" -> SmG3Scenario.run(context);
            case "play-crash-recovery" -> SmG1Scenario.run(context);
            case "worker" -> SmA8Scenario.run(context);
            case "spot-outbound" -> SmC2Scenario.run(context);
            case "spot-to-spot" -> SmC3Scenario.run(context);
            case "spot-mesh-cross-node" -> SmC4Scenario.run(context);
            case "idle-timer" -> SmE3Scenario.run(context);
            case "timer-overrun" -> SmE4Scenario.run(context);
            default -> throw new IllegalArgumentException("unknown client mode " + mode);
        }
    }
}
