using System;
using System.Collections.Generic;
using System.IO;
using UnrealBuildTool;

public class ZLinkStreamConnector : ModuleRules
{
    private const string PackageManifestName = "zlink-unreal-package.manifest";

    public ZLinkStreamConnector(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        ConfigureNativePackage(Target);
    }

    private void ConfigureNativePackage(ReadOnlyTargetRules Target)
    {
        var packageRoot = Environment.GetEnvironmentVariable("ZLINK_UNREAL_THIRDPARTY_ROOT");
        if (string.IsNullOrWhiteSpace(packageRoot))
        {
            packageRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/ZLink"));
        }
        else
        {
            packageRoot = Path.GetFullPath(packageRoot);
        }

        var manifestPath = Path.Combine(packageRoot, PackageManifestName);
        if (!File.Exists(manifestPath))
        {
            throw new BuildException(
                "ZLink Stream Connector native package is missing. "
                + "Run framework/languages/cpp/connector/engines/unreal/Tools/package-third-party.cmake "
                + "with ZLINK_UNREAL_BUILD_DIR and ZLINK_UNREAL_OUTPUT_DIR, or set "
                + "ZLINK_UNREAL_THIRDPARTY_ROOT to a prepared package. Expected: " + manifestPath);
        }

        var includes = new List<string>();
        var libraries = new List<string>();
        var runtimes = new List<string>();
        var systemLibraries = new List<string>();
        var schema = 0;
        var packagePlatform = string.Empty;
        var packageArchitecture = string.Empty;
        var packageConfiguration = string.Empty;
        var packageCompilerId = string.Empty;
        var packageCompilerVersion = string.Empty;
        var packageCxxStandard = 0;

        foreach (var rawLine in File.ReadAllLines(manifestPath))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line.StartsWith("#", StringComparison.Ordinal))
            {
                continue;
            }

            var separator = line.IndexOf('=');
            if (separator <= 0 || separator == line.Length - 1)
            {
                throw new BuildException("Invalid ZLink Unreal package manifest line: " + rawLine);
            }

            var key = line.Substring(0, separator).Trim();
            var value = line.Substring(separator + 1).Trim();
            switch (key)
            {
                case "schema":
                    if (!int.TryParse(value, out schema))
                    {
                        throw new BuildException("Invalid ZLink Unreal package manifest schema: " + value);
                    }
                    break;
                case "platform":
                    packagePlatform = value;
                    break;
                case "architecture":
                    packageArchitecture = value;
                    break;
                case "configuration":
                    packageConfiguration = value;
                    break;
                case "compiler_id":
                    packageCompilerId = value;
                    break;
                case "compiler_version":
                    packageCompilerVersion = value;
                    break;
                case "cxx_standard":
                    if (!int.TryParse(value, out packageCxxStandard))
                    {
                        throw new BuildException("Invalid ZLink Unreal package C++ standard: " + value);
                    }
                    break;
                case "include":
                    includes.Add(ResolvePackageDirectory(packageRoot, value));
                    break;
                case "library":
                    libraries.Add(ResolvePackageFile(packageRoot, value));
                    break;
                case "runtime":
                    runtimes.Add(ResolvePackageFile(packageRoot, value));
                    break;
                case "system_library":
                    systemLibraries.Add(value);
                    break;
                default:
                    throw new BuildException("Unknown ZLink Unreal package manifest key: " + key);
            }
        }

        if (schema != 1 || includes.Count == 0 || libraries.Count == 0
            || string.IsNullOrWhiteSpace(packagePlatform)
            || string.IsNullOrWhiteSpace(packageArchitecture)
            || string.IsNullOrWhiteSpace(packageConfiguration)
            || string.IsNullOrWhiteSpace(packageCompilerId)
            || string.IsNullOrWhiteSpace(packageCompilerVersion)
            || packageCxxStandard != 20)
        {
            throw new BuildException(
                "ZLink Unreal package manifest must declare schema=1, target metadata, "
                + "cxx_standard=20, at least one include, and at least one library: "
                + manifestPath);
        }

        ValidateTargetMetadata(
            Target,
            packagePlatform,
            packageArchitecture,
            packageConfiguration,
            packageCompilerId,
            packageCompilerVersion);

        PublicIncludePaths.AddRange(includes);
        PublicAdditionalLibraries.AddRange(libraries);
        PublicSystemLibraries.AddRange(systemLibraries);
        foreach (var runtime in runtimes)
        {
            RuntimeDependencies.Add(runtime);
        }
    }

    private static void ValidateTargetMetadata(
        ReadOnlyTargetRules Target,
        string packagePlatform,
        string packageArchitecture,
        string packageConfiguration,
        string packageCompilerId,
        string packageCompilerVersion)
    {
        var targetPlatform = NormalizePlatform(Target.Platform.ToString());
        var targetArchitecture = NormalizeArchitecture(Target.Architecture.ToString());
        var targetConfiguration = NormalizeConfiguration(Target.Configuration.ToString());
        if (!string.Equals(targetPlatform, NormalizePlatform(packagePlatform), StringComparison.Ordinal)
            || !string.Equals(targetArchitecture, NormalizeArchitecture(packageArchitecture), StringComparison.Ordinal)
            || !string.Equals(targetConfiguration, NormalizeConfiguration(packageConfiguration), StringComparison.Ordinal))
        {
            throw new BuildException(
                "ZLink Unreal native package target mismatch. Package="
                + packagePlatform + "/" + packageArchitecture + "/" + packageConfiguration
                + ", target=" + Target.Platform + "/" + Target.Architecture + "/" + Target.Configuration);
        }

        var expectedCompilerId = Environment.GetEnvironmentVariable("ZLINK_UNREAL_COMPILER_ID");
        if (string.IsNullOrWhiteSpace(expectedCompilerId))
        {
            throw new BuildException(
                "ZLink Unreal native package compiler validation requires "
                + "ZLINK_UNREAL_COMPILER_ID to match manifest compiler_id=" + packageCompilerId);
        }
        if (!string.Equals(expectedCompilerId, packageCompilerId, StringComparison.OrdinalIgnoreCase))
        {
            throw new BuildException(
                "ZLink Unreal native package compiler mismatch. Package=" + packageCompilerId
                + ", expected=" + expectedCompilerId);
        }
        var expectedCompilerVersion = Environment.GetEnvironmentVariable("ZLINK_UNREAL_COMPILER_VERSION");
        if (string.IsNullOrWhiteSpace(expectedCompilerVersion))
        {
            throw new BuildException(
                "ZLink Unreal native package compiler validation requires "
                + "ZLINK_UNREAL_COMPILER_VERSION to match manifest compiler_version="
                + packageCompilerVersion);
        }
        if (!string.Equals(expectedCompilerVersion, packageCompilerVersion, StringComparison.OrdinalIgnoreCase))
        {
            throw new BuildException(
                "ZLink Unreal native package compiler version mismatch. Package="
                + packageCompilerVersion + ", expected=" + expectedCompilerVersion);
        }
    }

    private static string NormalizePlatform(string value)
    {
        var normalized = value.Trim().ToLowerInvariant();
        if (normalized == "win64" || normalized == "windows")
        {
            return "windows";
        }
        if (normalized == "mac" || normalized == "macos" || normalized == "darwin")
        {
            return "darwin";
        }
        return normalized;
    }

    private static string NormalizeArchitecture(string value)
    {
        var normalized = value.Trim().ToLowerInvariant().Replace("-", string.Empty).Replace("_", string.Empty);
        if (normalized == "x64" || normalized == "amd64" || normalized == "x8664")
        {
            return "x86_64";
        }
        if (normalized == "arm64" || normalized == "aarch64")
        {
            return "arm64";
        }
        return normalized;
    }

    private static string NormalizeConfiguration(string value)
    {
        var normalized = value.Trim().ToLowerInvariant();
        return normalized == "debug" ? "debug" : "release";
    }

    private static string ResolvePackageDirectory(string packageRoot, string relativePath)
    {
        var path = ResolvePackagePath(packageRoot, relativePath);
        if (!Directory.Exists(path))
        {
            throw new BuildException("ZLink Unreal package include directory is missing: " + path);
        }
        return path;
    }

    private static string ResolvePackageFile(string packageRoot, string relativePath)
    {
        var path = ResolvePackagePath(packageRoot, relativePath);
        if (!File.Exists(path))
        {
            throw new BuildException("ZLink Unreal package library/runtime is missing: " + path);
        }
        return path;
    }

    private static string ResolvePackagePath(string packageRoot, string relativePath)
    {
        if (Path.IsPathRooted(relativePath))
        {
            throw new BuildException("ZLink Unreal package manifest paths must be relative: " + relativePath);
        }

        var root = Path.GetFullPath(packageRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var path = Path.GetFullPath(Path.Combine(packageRoot, relativePath));
        if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
        {
            throw new BuildException("ZLink Unreal package manifest path escapes its package root: " + relativePath);
        }
        return path;
    }
}
