package systems.zlink.e2e.registrationcodec.client;

import systems.zlink.e2e.registrationcodec.client.Scenarios.AttributeRegistrationScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.AutoRegistrationScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.CodecMismatchScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.InvalidRegistrationScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.ManualRegistrationScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.RcA4DiLifecycleScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.RcA5FilterOrderingScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.RcB1JsonCodecScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.RcB2ProtobufCodecScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.RcB3MessagePackCodecScenario;
import systems.zlink.e2e.registrationcodec.client.Scenarios.RcB4CodecCoexistenceScenario;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;

public final class ScenarioRunner {
    private final ScenarioContext context;

    public ScenarioRunner(ScenarioContext context) {
        this.context = context;
    }

    public void run() {
        String scenario = context.options().scenario();
        if (!"all".equals(scenario)) {
            runOne(scenario);
            return;
        }
        InvalidRegistrationScenario.run(context);
        AutoRegistrationScenario.run(context);
        AttributeRegistrationScenario.run(context);
        ManualRegistrationScenario.run(context);
        RcA4DiLifecycleScenario.run(context);
        RcA5FilterOrderingScenario.run(context);
        RcB1JsonCodecScenario.run(context);
        RcB2ProtobufCodecScenario.run(context);
        RcB3MessagePackCodecScenario.run(context);
        RcB4CodecCoexistenceScenario.run(context);
        CodecMismatchScenario.run(context);
    }

    private void runOne(String scenario) {
        switch (scenario) {
            case "RC-A1" -> AutoRegistrationScenario.run(context);
            case "RC-A2" -> AttributeRegistrationScenario.run(context);
            case "RC-A3" -> ManualRegistrationScenario.run(context);
            case "RC-A4" -> RcA4DiLifecycleScenario.run(context);
            case "RC-A5" -> RcA5FilterOrderingScenario.run(context);
            case "RC-A6" -> InvalidRegistrationScenario.run(context);
            case "RC-B1" -> RcB1JsonCodecScenario.run(context);
            case "RC-B2" -> RcB2ProtobufCodecScenario.run(context);
            case "RC-B3" -> RcB3MessagePackCodecScenario.run(context);
            case "RC-B4" -> RcB4CodecCoexistenceScenario.run(context);
            case "RC-B5" -> CodecMismatchScenario.run(context);
            default -> throw new IllegalArgumentException("Unknown RegistrationCodec scenario '" + scenario + "'.");
        }
    }
}
