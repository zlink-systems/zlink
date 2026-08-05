using Microsoft.Extensions.Configuration;

namespace DeliveryDispatch.Server.Configuration;

/// <summary>
/// The values that belong to this one role: what it is called, where it writes, and — for a
/// courier actor node — which node it is. A role never learns about the others' internals; it
/// gets the topology it has to dial and nothing more.
/// </summary>
public sealed class SampleRoleOptions
{
    public string Name { get; set; } = string.Empty;

    public string LogDir { get; set; } = string.Empty;

    public string WorkDir { get; set; } = string.Empty;

}

/// <summary>
/// The whole of this role's configuration, read once from the file the runner wrote and validated
/// before the framework runtime starts. Nothing below this boundary reads a file or an environment
/// variable — the shell does not get to be the sample's configuration system
/// (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md §2).
/// </summary>
public sealed record SampleConfiguration(SampleTopology Topology, SampleRoleOptions Role)
{
    public string EvidencePath => Path.Combine(Role.WorkDir, "events.log");

    /// <summary>
    /// Reads <c>--config &lt;path&gt;</c> and nothing else. A missing file or a missing required
    /// value fails here, before anything is bound or connected (§2.1).
    /// </summary>
    public static SampleConfiguration Load(string[] args)
    {
        var path = ReadOption(args, "--config")
                   ?? throw new ArgumentException(
                       "DeliveryDispatch roles require --config <path>.");
        if (!File.Exists(path))
            throw new FileNotFoundException($"Configuration file was not found: {path}", path);

        var configuration = new ConfigurationBuilder()
            .AddJsonFile(path, optional: false, reloadOnChange: false)
            .Build();

        var role = configuration.GetSection("sample:role").Get<SampleRoleOptions>()
                   ?? throw new InvalidOperationException("sample.role is missing.");
        var topology = configuration.GetSection("sample:topology").Get<SampleTopologyOptions>()
                       ?? throw new InvalidOperationException("sample.topology is missing.");

        Require(role.Name, "sample.role.name");
        Require(role.LogDir, "sample.role.logDir");
        Require(role.WorkDir, "sample.role.workDir");

        return new SampleConfiguration(topology.ToTopology(role.Name), role);
    }

    private static void Require(string value, string key)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"{key} is required.");
    }

    private static string? ReadOption(string[] args, string name)
    {
        var index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
    }
}
