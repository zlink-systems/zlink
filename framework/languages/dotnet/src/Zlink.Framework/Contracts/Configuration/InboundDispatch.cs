namespace Zlink.Framework.Contracts.Configuration;

/// <summary>
/// Selects how much of the process memory limit may hold application payloads
/// that have been received but whose handlers have not completed.
/// </summary>
public enum ZLinkApplicationHwmProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

/// <summary>
/// Configures the host-wide admission limit for inbound application payloads.
/// </summary>
public interface IZLinkInboundDispatchOptions
{
    /// <summary>
    /// Gets or sets the host-wide pending payload limit in bytes. A null value
    /// selects automatic sizing, zero disables this limit, and a positive value
    /// applies that exact limit.
    /// </summary>
    ulong? ApplicationHwmBytes { get; set; }

    /// <summary>
    /// Gets or sets the profile used by automatic sizing. The default is
    /// <see cref="ZLinkApplicationHwmProfile.Balanced"/>.
    /// </summary>
    ZLinkApplicationHwmProfile ApplicationHwmProfile { get; set; }

    /// <summary>
    /// Gets or sets the finite process memory limit used by automatic sizing.
    /// When omitted, the Framework reads the container or cgroup limit.
    /// </summary>
    ulong? ProcessMemoryLimitBytes { get; set; }
}
