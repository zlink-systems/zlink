using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.Versioning;
using Systems.Zlink;

namespace Zlink.Framework.Tests.Common;

internal static class FrameworkTestEnvironment
{
    public static string GetFrameworkRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);

        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "Zlink.Framework.sln"))) return current.FullName;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not locate framework/languages/dotnet root.");
    }

    public static string GetRepoRoot()
    {
        var frameworkRoot = new DirectoryInfo(GetFrameworkRoot());
        var repoRoot = frameworkRoot.Parent?.Parent?.Parent;

        if (repoRoot is null)
            throw new DirectoryNotFoundException("Could not locate repository root from framework root.");

        return repoRoot.FullName;
    }

    public static string GetTargetFrameworkMoniker()
    {
        var attribute = Assembly.GetExecutingAssembly().GetCustomAttribute<TargetFrameworkAttribute>()
                        ?? throw new InvalidOperationException("Target framework attribute is missing.");
        var frameworkName = new FrameworkName(attribute.FrameworkName);
        return string.Create(
            CultureInfo.InvariantCulture,
            $"net{frameworkName.Version.Major}.{frameworkName.Version.Minor}");
    }

    public static string GetBuildConfiguration()
    {
        return Assembly.GetExecutingAssembly()
            .GetCustomAttribute<AssemblyConfigurationAttribute>()?.Configuration ?? "Debug";
    }

    public static string GetTestHostProjectPath()
    {
        return Path.Combine(
            GetFrameworkRoot(),
            "testapps",
            "Zlink.Framework.TestHost",
            "Zlink.Framework.TestHost.csproj");
    }

    public static string GetTestHostAssemblyPath()
    {
        return Path.Combine(
            GetFrameworkRoot(),
            "testapps",
            "Zlink.Framework.TestHost",
            "bin",
            GetBuildConfiguration(),
            GetTargetFrameworkMoniker(),
            "Zlink.Framework.TestHost.dll");
    }

    public static string GetTestHostExecutablePath()
    {
        return Path.Combine(
            GetFrameworkRoot(),
            "testapps",
            "Zlink.Framework.TestHost",
            "bin",
            GetBuildConfiguration(),
            GetTargetFrameworkMoniker(),
            OperatingSystem.IsWindows() ? "Zlink.Framework.TestHost.exe" : "Zlink.Framework.TestHost");
    }

    public static string GetDotNetHostPath()
    {
        const string localHostPath = "/home/hep7/.dotnet/dotnet";

        if (File.Exists(localHostPath)) return localHostPath;

        var hostPath = Environment.GetEnvironmentVariable("DOTNET_HOST_PATH");

        if (!string.IsNullOrWhiteSpace(hostPath) && File.Exists(hostPath)) return hostPath;

        return "dotnet";
    }

    public static string CreateTestLogDirectory(string prefix)
    {
        var directory = Path.Combine(
            Path.GetTempPath(),
            "zlink-framework-tests",
            $"{prefix}-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        return directory;
    }

    public static ProcessStartInfo CreateTestHostStartInfo(
        string? readyFilePath = null,
        IReadOnlyList<string>? additionalArguments = null,
        bool redirectStandardInput = true)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = GetDotNetHostPath(),
            RedirectStandardInput = redirectStandardInput,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };

        startInfo.ArgumentList.Add(GetTestHostAssemblyPath());

        if (!string.IsNullOrWhiteSpace(readyFilePath))
        {
            startInfo.ArgumentList.Add("--ready-file");
            startInfo.ArgumentList.Add(readyFilePath);
        }

        if (additionalArguments is not null)
            foreach (var argument in additionalArguments)
                startInfo.ArgumentList.Add(argument);

        return startInfo;
    }
}

internal sealed class TestContext : IContext
{
    public IContextOptions Options =>
        throw new NotSupportedException("The test context does not create native sockets.");

    public IPairSocket CreatePairSocket() => throw NotSupported();

    public IDealerSocket CreateDealerSocket() => throw NotSupported();

    public IRouterSocket CreateRouterSocket() => throw NotSupported();

    public IPubSocket CreatePubSocket() => throw NotSupported();

    public ISubSocket CreateSubSocket() => throw NotSupported();

    public IXPubSocket CreateXPubSocket() => throw NotSupported();

    public IXSubSocket CreateXSubSocket() => throw NotSupported();

    public IStreamSocket CreateStreamSocket() => throw NotSupported();

    public void Shutdown()
    {
    }

    public void RecalculateAutoHwm()
    {
    }

    public void Dispose()
    {
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;

    private static NotSupportedException NotSupported() =>
        new("The test context does not create native sockets.");
}
