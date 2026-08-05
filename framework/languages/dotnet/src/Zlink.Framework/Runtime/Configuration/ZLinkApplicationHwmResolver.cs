using System.Globalization;
using System.Runtime.InteropServices;

namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkApplicationHwmResolver
{
    private static readonly string[] RootCgroupLimitFiles =
    [
        "/sys/fs/cgroup/memory.max",
        "/sys/fs/cgroup/memory/memory.limit_in_bytes"
    ];

    public static ulong Resolve(ZLinkInboundDispatchOptionsModel options)
    {
        if (options.ApplicationHwmBytes is { } configured)
            return configured;

        return Resolve(
            options,
            ReadOsMemoryLimit(),
            ReadManagedHeapLimit(),
            ReadTotalPhysicalMemory());
    }

    internal static ulong Resolve(
        ZLinkInboundDispatchOptionsModel options,
        ulong? osLimit,
        ulong? managedHeapLimit,
        ulong? physicalMemoryLimit)
    {
        if (options.ApplicationHwmBytes is { } configured)
            return configured;

        // Spec 06: configured limit, then the smaller of the OS and managed-heap
        // limits, then total physical memory. Total, not free, keeps Auto
        // deterministic even when the host exposes no finite process limit.
        var finiteLimit = options.ProcessMemoryLimitBytes
                          ?? Smaller(osLimit, managedHeapLimit)
                          ?? physicalMemoryLimit;
        if (finiteLimit is null)
            throw new ZLinkConfigurationException(
                "Application Auto HWM could not read a finite memory budget.");
        var resolvedLimit = finiteLimit.Value;

        var percent = options.ApplicationHwmProfile switch
        {
            ZLinkApplicationHwmProfile.Compact => 2UL,
            ZLinkApplicationHwmProfile.LowLatency => 5UL,
            ZLinkApplicationHwmProfile.Balanced => 10UL,
            ZLinkApplicationHwmProfile.Throughput => 20UL,
            _ => throw new ZLinkConfigurationException(
                $"Unknown ApplicationHwmProfile value '{(int)options.ApplicationHwmProfile}'.")
        };

        // Compute floor(limit * percent / 100) without overflowing UInt64.
        var hwm = resolvedLimit / 100UL * percent
                  + resolvedLimit % 100UL * percent / 100UL;
        if (hwm == 0)
            throw new ZLinkConfigurationException(
                "Application Auto HWM must resolve to a positive finite byte value.");
        return hwm;
    }

    internal static ulong? ReadManagedHeapLimit()
    {
        var total = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes;
        return total > 0 ? (ulong) total : null;
    }

    internal static ulong? ReadOsMemoryLimit()
    {
        var containerLimit = ReadCgroupMemoryLimit();
        var platformLimit = OperatingSystem.IsWindows()
            ? ReadWindowsJobObjectMemoryLimit()
            : ReadProcessAddressSpaceLimit();
        return Smaller(containerLimit, platformLimit);
    }

    internal static ulong? ReadTotalPhysicalMemory()
    {
        if (OperatingSystem.IsWindows())
        {
            var status = new MemoryStatusEx { Length = (uint) Marshal.SizeOf<MemoryStatusEx>() };
            return GlobalMemoryStatusEx(ref status) && status.TotalPhysicalMemory > 0
                ? status.TotalPhysicalMemory
                : null;
        }

        if (OperatingSystem.IsMacOS())
        {
            var macMemory = ReadMacOsPhysicalMemory();
            if (macMemory is not null)
                return macMemory;
        }

        try
        {
            if (!File.Exists("/proc/meminfo")) return null;
            foreach (var line in File.ReadLines("/proc/meminfo"))
            {
                if (!line.StartsWith("MemTotal:", StringComparison.Ordinal)) continue;
                var fields = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (fields.Length < 2
                    || !ulong.TryParse(fields[1], NumberStyles.None,
                        CultureInfo.InvariantCulture, out var kib)
                    || kib == 0)
                    return null;
                return checked(kib * 1024UL);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        catch (OverflowException)
        {
        }

        return null;
    }

    private static ulong? ReadMacOsPhysicalMemory()
    {
        try
        {
            var length = (nuint)Marshal.SizeOf<ulong>();
            if (SysctlByName(
                    "hw.memsize",
                    out var bytes,
                    ref length,
                    IntPtr.Zero,
                    0) != 0
                || length < (nuint)Marshal.SizeOf<ulong>()
                || bytes == 0)
                return null;
            return bytes;
        }
        catch (DllNotFoundException)
        {
            return null;
        }
        catch (EntryPointNotFoundException)
        {
            return null;
        }
        catch (BadImageFormatException)
        {
            return null;
        }
    }

    private static ulong? Smaller(ulong? first, ulong? second)
    {
        if (first is null) return second;
        if (second is null) return first;
        return Math.Min(first.Value, second.Value);
    }

    internal static ulong? ReadCgroupMemoryLimit()
    {
        ulong? smallest = null;
        foreach (var path in EnumerateCgroupLimitFiles())
        {
            string value;
            try
            {
                if (!File.Exists(path)) continue;
                value = File.ReadAllText(path).Trim();
            }
            catch (IOException)
            {
                continue;
            }
            catch (UnauthorizedAccessException)
            {
                continue;
            }

            if (string.Equals(value, "max", StringComparison.OrdinalIgnoreCase))
                continue;
            if (!ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var limit))
                continue;

            // cgroup v1 reports values near Int64.MaxValue when no finite limit exists.
            if (limit >= 0x7FFF_FFFF_FFFF_0000UL)
                continue;
            smallest = Smaller(smallest, limit);
        }

        return smallest;
    }

    private static ulong? ReadWindowsJobObjectMemoryLimit()
    {
        const uint ProcessMemoryLimitFlag = 0x0000_0100;
        const uint JobMemoryLimitFlag = 0x0000_0200;
        const int JobObjectExtendedLimitInformationClass = 9;

        try
        {
            var information = new JobObjectExtendedLimitInformation();
            if (!QueryInformationJobObject(
                    IntPtr.Zero,
                    JobObjectExtendedLimitInformationClass,
                    ref information,
                    (uint)Marshal.SizeOf<JobObjectExtendedLimitInformation>(),
                    out _))
                return null;

            var flags = information.BasicLimitInformation.LimitFlags;
            ulong? processLimit = (flags & ProcessMemoryLimitFlag) != 0
                ? information.ProcessMemoryLimit.ToUInt64()
                : null;
            ulong? jobLimit = (flags & JobMemoryLimitFlag) != 0
                ? information.JobMemoryLimit.ToUInt64()
                : null;
            return Smaller(processLimit, jobLimit);
        }
        catch (DllNotFoundException)
        {
            return null;
        }
        catch (EntryPointNotFoundException)
        {
            return null;
        }
        catch (BadImageFormatException)
        {
            return null;
        }
    }

    private static ulong? ReadProcessAddressSpaceLimit()
    {
        var resource = OperatingSystem.IsLinux()
            ? 9 // RLIMIT_AS on Linux
            : OperatingSystem.IsMacOS()
                ? 5 // RLIMIT_AS on Darwin
                : -1;
        if (resource < 0)
            return null;

        try
        {
            if (GetResourceLimit(resource, out var limit) != 0)
                return null;

            return Smaller(
                NormalizeRlimit(limit.Soft),
                NormalizeRlimit(limit.Hard));
        }
        catch (DllNotFoundException)
        {
            return null;
        }
        catch (EntryPointNotFoundException)
        {
            return null;
        }
        catch (BadImageFormatException)
        {
            return null;
        }
    }

    private static ulong? NormalizeRlimit(ulong value)
    {
        if (value == 0)
            return 0;
        if (value >= long.MaxValue)
            return null;
        return value;
    }

    private static IEnumerable<string> EnumerateCgroupLimitFiles()
    {
        string[] membershipLines = [];
        try
        {
            if (File.Exists("/proc/self/cgroup"))
                membershipLines = File.ReadAllLines("/proc/self/cgroup");
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }

        foreach (var line in membershipLines)
        {
            var fields = line.Split(':', 3);
            if (fields.Length != 3) continue;
            var relative = fields[2].TrimStart('/');
            if (fields[0] == "0")
                yield return Path.Combine("/sys/fs/cgroup", relative, "memory.max");
            else if (fields[1].Split(',').Contains("memory", StringComparer.Ordinal))
                yield return Path.Combine(
                    "/sys/fs/cgroup/memory",
                    relative,
                    "memory.limit_in_bytes");
        }

        foreach (var path in RootCgroupLimitFiles)
            yield return path;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MemoryStatusEx
    {
        internal uint Length;
        internal uint MemoryLoad;
        internal ulong TotalPhysicalMemory;
        internal ulong AvailablePhysicalMemory;
        internal ulong TotalPageFile;
        internal ulong AvailablePageFile;
        internal ulong TotalVirtual;
        internal ulong AvailableVirtual;
        internal ulong AvailableExtendedVirtual;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectBasicLimitInformation
    {
        internal long PerProcessUserTimeLimit;
        internal long PerJobUserTimeLimit;
        internal uint LimitFlags;
        internal UIntPtr MinimumWorkingSetSize;
        internal UIntPtr MaximumWorkingSetSize;
        internal uint ActiveProcessLimit;
        internal UIntPtr Affinity;
        internal uint PriorityClass;
        internal uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        internal ulong ReadOperationCount;
        internal ulong WriteOperationCount;
        internal ulong OtherOperationCount;
        internal ulong ReadTransferCount;
        internal ulong WriteTransferCount;
        internal ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectExtendedLimitInformation
    {
        internal JobObjectBasicLimitInformation BasicLimitInformation;
        internal IoCounters IoInfo;
        internal UIntPtr ProcessMemoryLimit;
        internal UIntPtr JobMemoryLimit;
        internal UIntPtr PeakProcessMemoryUsed;
        internal UIntPtr PeakJobMemoryUsed;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ResourceLimit
    {
        internal ulong Soft;
        internal ulong Hard;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx status);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryInformationJobObject(
        IntPtr job,
        int informationClass,
        ref JobObjectExtendedLimitInformation information,
        uint informationLength,
        out uint returnLength);

    [DllImport("libc", EntryPoint = "getrlimit", SetLastError = true)]
    private static extern int GetResourceLimit(
        int resource,
        out ResourceLimit limit);

    [DllImport("libc", EntryPoint = "sysctlbyname", SetLastError = true)]
    private static extern int SysctlByName(
        [MarshalAs(UnmanagedType.LPStr)] string name,
        out ulong oldValue,
        ref nuint oldLength,
        IntPtr newValue,
        nuint newLength);
}
