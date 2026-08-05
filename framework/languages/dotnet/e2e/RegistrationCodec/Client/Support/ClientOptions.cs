using Zlink.Framework.E2E.Configuration;
namespace RegistrationCodec.Client.Support;

internal sealed record ClientOptions(
    string ChannelEndpoint,
    string ServerUrl,
    string ScenarioRequesterUrl,
    string CodecRequesterUrl,
    string InvalidServerProject,
    string ConfigDir,
    string Scenario,
    string LogDir)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
