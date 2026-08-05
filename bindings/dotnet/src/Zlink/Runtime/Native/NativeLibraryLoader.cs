using System.Reflection;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

internal static class NativeLibraryLoader
{
    private const string LinuxUnversionedName = "libzlink.so";
    private const string LinuxSoname = "libzlink.so.11";

    private static readonly object Sync = new();
    private static readonly ConcurrentDictionary<string, bool> ExportCache = new();
    private static IntPtr _handle = IntPtr.Zero;
    private static bool _resolverInstalled;
    private static bool _exportsValidated;

#pragma warning disable CA2255
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Initialize()
    {
        EnsureResolverInstalled();
    }

    internal static void EnsureLoaded()
    {
        EnsureResolverInstalled();
        if (_handle != IntPtr.Zero)
            return;

        lock (Sync)
        {
            if (_handle != IntPtr.Zero)
                return;

            if (TryLoadConfiguredPath(out _handle))
            {
                ValidateRequiredExports();
                return;
            }

            if (TryLoadPackagedCandidates(out _handle))
            {
                ValidateRequiredExports();
                return;
            }

            throw new DllNotFoundException(
                "The packaged Core 11 runtime was not found. Set ZLINK_LIBRARY_PATH only when testing an approved Core 11 runtime directly.");
        }
    }

    private static void EnsureResolverInstalled()
    {
        if (_resolverInstalled)
            return;

        lock (Sync)
        {
            if (_resolverInstalled)
                return;
            NativeLibrary.SetDllImportResolver(typeof(NativeLibraryLoader).Assembly,
                Resolve);
            _resolverInstalled = true;
        }
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly,
        DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, "zlink", StringComparison.Ordinal)
            && !string.Equals(libraryName, "libzlink", StringComparison.Ordinal))
            return IntPtr.Zero;
        EnsureLoaded();
        return _handle;
    }

    private static bool TryLoadConfiguredPath(out IntPtr handle)
    {
        handle = IntPtr.Zero;
        var path = Environment.GetEnvironmentVariable("ZLINK_LIBRARY_PATH");
        if (string.IsNullOrWhiteSpace(path))
            return false;
        return TryLoad(path, out handle);
    }

    private static bool TryLoadPackagedCandidates(out IntPtr handle)
    {
        handle = IntPtr.Zero;
        var rid = GetRid();
        foreach (var baseDir in GetBaseDirs())
        foreach (var candidate in GetCandidates(baseDir, rid))
        {
            if (!File.Exists(candidate))
                continue;
            if (TryLoad(candidate, out handle))
                return true;
        }

        return false;
    }

    internal static bool HasExport(string symbol)
    {
        if (string.IsNullOrWhiteSpace(symbol))
            throw new ArgumentException("Symbol name is required.",
                nameof(symbol));

        EnsureLoaded();
        return ExportCache.GetOrAdd(symbol, HasLoadedExport);
    }

    internal static List<string> GetMissingExports(IEnumerable<string> symbols)
    {
        var missing = new List<string>();
        foreach (var symbol in symbols)
            if (!HasExport(symbol))
                missing.Add(symbol);
        return missing;
    }

    internal static List<string> GetMissingExports(ReadOnlySpan<string> symbols)
    {
        var missing = new List<string>();
        foreach (var symbol in symbols)
            if (!HasExport(symbol))
                missing.Add(symbol);
        return missing;
    }

    private static bool HasLoadedExport(string symbol)
    {
        return _handle != IntPtr.Zero
               && NativeLibrary.TryGetExport(_handle, symbol, out _);
    }

    private static void ValidateRequiredExports()
    {
        if (_exportsValidated || _handle == IntPtr.Zero)
            return;

        foreach (var export in NativeMethods.RequiredExports)
        {
            if (NativeLibrary.TryGetExport(_handle, export, out _))
                continue;

            throw new DllNotFoundException(
                $"Loaded zlink library is missing required export '{export}'.");
        }

        _exportsValidated = true;
    }

    private static bool TryLoad(string nameOrPath, out IntPtr handle)
    {
        try
        {
            handle = NativeLibrary.Load(nameOrPath);
            return true;
        }
        catch
        {
            handle = IntPtr.Zero;
            return false;
        }
    }

    private static IEnumerable<string> GetCandidates(string baseDir, string rid)
    {
        var libNames = GetLibNames();
        foreach (var libName in libNames)
        {
            yield return Path.Combine(baseDir, "runtimes", rid, "native", libName);
        }
    }

    private static IEnumerable<string> GetBaseDirs()
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var appBase = AppContext.BaseDirectory;
        if (!string.IsNullOrEmpty(appBase) && seen.Add(appBase))
            yield return appBase;

        var assemblyBase =
            Path.GetDirectoryName(typeof(NativeLibraryLoader).Assembly.Location);
        if (!string.IsNullOrEmpty(assemblyBase) && seen.Add(assemblyBase))
            yield return assemblyBase;

        var entryBase = Path.GetDirectoryName(Assembly.GetEntryAssembly()?.Location);
        if (!string.IsNullOrEmpty(entryBase) && seen.Add(entryBase))
            yield return entryBase;
    }

    private static string GetRid()
    {
        var arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.Arm64 => "arm64",
            Architecture.X64 => "x64",
            Architecture.X86 => "x86",
            _ => RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant()
        };
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return $"win-{arch}";
        if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            return $"osx-{arch}";
        return $"linux-{arch}";
    }

    private static string[] GetLibNames()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return new[] { "zlink.dll" };
        if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            return new[] { "libzlink.dylib" };
        return new[] { LinuxUnversionedName, LinuxSoname };
    }
}
