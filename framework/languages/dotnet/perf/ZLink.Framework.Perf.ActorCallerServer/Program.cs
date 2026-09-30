using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.role != "actor-caller" || !config.source) throw new ArgumentException("ActorCallerServer runs the source role of §10.9 and §10.10.");
await (config.scenario switch
{
    "actor-no-bind-request-echo" => ActorNoBindRequestEchoScenario.RunAsync(config),
    "actor-no-bind-send-send-echo" => ActorNoBindSendSendEchoScenario.RunAsync(config),
    _ => throw new ArgumentException($"ActorCallerServer does not run scenario '{config.scenario}'.")
});
