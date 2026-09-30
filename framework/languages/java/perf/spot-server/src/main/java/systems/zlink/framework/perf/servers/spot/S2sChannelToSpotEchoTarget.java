package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.List;
import java.util.concurrent.CompletionStage;

// The Spot process of §10.3 and §10.4 (Object Server, no driver): User Spots that answer a request with a typed echo
// (§10.3) or answer a send by sending the echo to the caller's return Channel (§10.4). The measured operation lives in the
// Channel process; this side only echoes.
public final class S2sChannelToSpotEchoTarget {
    private S2sChannelToSpotEchoTarget() {}

    public static void run(RoleConfig config) {
        boolean request = "s2s-channel-to-spot-request-echo".equals(config.scenario());
        ServerApplication app = request
                ? SpotRole.application(config, PerfEchoSpot.class, false, null)
                : SpotRole.application(config, S2sSendEchoSpot.class, true, null); // the echo goes to the caller return ChannelName
        app.bean(ScenarioMetrics.class, () -> new ScenarioMetrics(app.measurement()).spotInternalsUnsupported());
        var context = app.start();
        SpotRole.createSpots(config, context.getBean(ZLinkSpotManager.class), context.getBean(ZLinkRouteMeshRuntime.class),
                app.measurement()).thenAccept(objects -> {
                    if (objects != null) {
                        SpotRole.publish(context.getBean(ObjectsReadiness.class), objects);
                    }
                });
    }

    public static final class S2sSendEchoSpot extends PerfSpot {
        public S2sSendEchoSpot(ZLinkSpotContext context) {
            super(context);
            context.handlers().addHandler(S2sSendEchoHandler.class);
        }
    }

    // §10.4: the echo goes back as a second one-way send to the caller's own return ChannelName (in the DTO).
    public static final class S2sSendEchoHandler implements ZLinkSpotPacketHandler<S2sSendEchoSpot, PerfEchoRequest> {
        private final Measurement measurement;

        public S2sSendEchoHandler(Measurement measurement) {
            this.measurement = measurement;
        }

        @Override
        public CompletionStage<Void> handle(S2sSendEchoSpot spot, PerfEchoRequest message) {
            long received = PerfClock.now();
            measurement.handlerEnter();
            try {
                measurement.validateRequest(message, message.returnChannel(), null);
                if (message.returnChannel() == null || message.returnChannel().isEmpty()) {
                    throw new PerfValidationException("IdentityMismatch", "No return Channel in the request.");
                }
                PerfEchoReply reply = PayloadPattern.reply(message, received);
                measurement.recordApplicationCall(message, "send");
                return spot.context().outbound().sendToChannel(message.returnChannel(), reply).submit().whenComplete((ignored, error) -> {
                    if (error == null && "setup".equals(measurement.phase())) {
                        measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                                "ZLinkSpotPacketHandler<PerfEchoRequest> -> sendToChannel", message.correlationId())));
                    }
                    if (error != null) {
                        measurement.recordDiagnostic(error);
                    }
                    measurement.handlerExit();
                });
            } catch (RuntimeException error) {
                measurement.recordDiagnostic(error);
                measurement.handlerExit();
                throw error;
            }
        }
    }
}
