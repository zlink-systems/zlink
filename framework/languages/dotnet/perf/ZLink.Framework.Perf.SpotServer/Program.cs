using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "spot") throw new ArgumentException("SpotServer runs the spot role.");
switch (config.scenario)
{
    case "s2s-channel-to-spot-request-echo":
    case "s2s-channel-to-spot-send-send-echo": await S2sChannelToSpotEchoTarget.RunAsync(config); break;
    case "s2s-spot-to-channel-request-echo": await S2sSpotToChannelRequestEchoScenario.RunAsync(config); break;
    case "s2s-spot-to-channel-send-send-echo": await S2sSpotToChannelSendSendEchoScenario.RunAsync(config); break;
    case "spot-no-await-echo": await SpotNoAwaitEchoScenario.RunAsync(config); break;
    case "spot-worker-offload-echo": await SpotWorkerOffloadEchoScenario.RunAsync(config); break;
    default: throw new ArgumentException($"SpotServer does not run scenario {config.scenario}.");
}
