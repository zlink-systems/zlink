using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
switch (config.scenario)
{
    case "channel-echo-only": await ChannelEchoOnlyHost.RunAsync(config); break;
    case "s2s-channel-to-spot-request-echo": await S2sChannelToSpotRequestEchoScenario.RunAsync(config); break;
    case "s2s-channel-to-spot-send-send-echo": await S2sChannelToSpotSendSendEchoScenario.RunAsync(config); break;
    case "s2s-spot-to-channel-request-echo": await S2sSpotToChannelRequestEchoTarget.RunAsync(config); break;
    case "s2s-spot-to-channel-send-send-echo": await S2sSpotToChannelSendSendEchoTarget.RunAsync(config); break;
    default: throw new ArgumentException($"ChannelServer does not run scenario {config.scenario}.");
}
