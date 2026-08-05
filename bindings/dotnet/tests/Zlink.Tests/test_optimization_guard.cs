using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_optimization_guard
{
    // Match modifier words structurally instead of maintaining a modifier
    // allow-list. This keeps valid forms such as `public unsafe class` and a
    // nested `public new class` visible to the source-boundary gate.
    private static readonly Regex PublicNamedTypeDeclaration = new(
        @"\bpublic\b(?<Modifiers>(?:\s+[A-Za-z_][A-Za-z0-9_]*)*)\s+" +
        @"(?<Kind>class|struct|record|interface|enum)\b" +
        @"(?:\s+(?:class|struct))?\s+(?<Name>[A-Za-z_][A-Za-z0-9_]*)",
        RegexOptions.Compiled);

    private static readonly Regex PublicDelegateDeclaration = new(
        @"\bpublic\b(?<Modifiers>(?:\s+[A-Za-z_][A-Za-z0-9_]*)*)\s+" +
        @"delegate\b[^;{}=]*?\b(?<Name>[A-Za-z_][A-Za-z0-9_]*)" +
        @"\s*(?:<[^;{}()]*>)?\s*\(",
        RegexOptions.Compiled);

    [Fact]
    public void runtime_source_does_not_use_dynamic_interop_workarounds()
    {
        string source = ReadZlinkSource();

        Assert.DoesNotContain("GetMethod(", source, StringComparison.Ordinal);
        Assert.DoesNotContain("GetField(", source, StringComparison.Ordinal);
        Assert.DoesNotContain("BindingFlags.NonPublic", source, StringComparison.Ordinal);
        Assert.DoesNotContain("MethodInfo.Invoke", source, StringComparison.Ordinal);
        Assert.DoesNotContain("FieldInfo.GetValue", source, StringComparison.Ordinal);
    }

    [Fact]
    public void publish_topic_cache_encodes_null_terminated_utf8_without_temp_string()
    {
        string source = ReadZlinkSource();

        Assert.Contains("PublishTopicEncoding.GetNullTerminatedUtf8", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("topic + '\\0'", source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void topic_message_writable_buffer_receive_does_not_reset_topic_twice()
    {
        string source = ReadZlinkSource();

        Assert.Contains("ResetForReuse(false)", source,
            StringComparison.Ordinal);
        Assert.Contains("avoiding a transient empty", source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void samples_and_perf_use_only_public_binding_contracts()
    {
        string source = ReadSampleAndPerfSource();
        string[] forbidden =
        {
            "Systems.Zlink.Native",
            "Systems.Zlink.Sockets.Internal",
            "Systems.Zlink.Runtime",
            "NativeMethods",
            "NativeLibraryLoader",
            "DllImport(",
            "LibraryImport(",
            "BindingFlags.NonPublic",
            "InternalsVisibleTo",
            "FromNative",
            "MoveFromNative"
        };

        var violations = new List<string>();
        foreach (string token in forbidden)
        {
            if (source.Contains(token, StringComparison.Ordinal))
                violations.Add(token);
        }

        Assert.Empty(violations);
    }

    [Fact]
    public void internal_runtime_namespaces_stay_under_runtime()
    {
        string source = ReadZlinkSource();
        string[] forbidden =
        {
            "namespace Systems.Zlink.Native",
            "namespace Systems.Zlink.Sockets.Internal",
            "using Systems.Zlink.Native",
            "using Systems.Zlink.Sockets.Internal"
        };

        var violations = new List<string>();
        foreach (string token in forbidden)
        {
            if (source.Contains(token, StringComparison.Ordinal))
                violations.Add(token);
        }

        Assert.Empty(violations);
    }

    [Fact]
    public void public_contract_source_does_not_depend_on_runtime_namespaces()
    {
        string source = ReadSourceTree(Path.Combine(
            BindingRoot(), "src", "Zlink", "Contracts"));

        Assert.DoesNotContain("using Systems.Zlink.Runtime", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Systems.Zlink.Runtime.", source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void runtime_public_declarations_are_partial_implementations_of_contract_types()
    {
        string bindingRoot = BindingRoot();
        var contractTypes = ReadPublicTypeDeclarations(
            Path.Combine(bindingRoot, "src", "Zlink", "Contracts"));
        string runtimeRoot = Path.Combine(bindingRoot, "src", "Zlink", "Runtime");
        var runtimeTypes = ReadPublicTypeDeclarationLocations(runtimeRoot);
        var violations = FindRuntimePublicDeclarationViolations(
            contractTypes, runtimeTypes);

        Assert.Empty(violations);
    }

    [Theory]
    [InlineData("", "public unsafe class RuntimeLeak { }")]
    [InlineData("", "public class Owner { public new class RuntimeLeak { } }")]
    [InlineData("public enum RuntimeLeak { Value = 0 }",
        "public enum RuntimeLeak { Value = 0 }")]
    public void runtime_public_declaration_scanner_rejects_modifier_and_nested_loopholes(
        string contractSource, string runtimeSource)
    {
        var declarations = ReadPublicTypeDeclarations(runtimeSource,
            "mutation.cs").ToList();
        var contractTypes = ReadPublicTypeDeclarations(contractSource,
                "contract.cs")
            .Select(declaration => declaration.TypeName)
            .ToHashSet(StringComparer.Ordinal);
        var violations = FindRuntimePublicDeclarationViolations(
            contractTypes, declarations);

        Assert.Contains("mutation.cs:RuntimeLeak", violations);
    }

    private static string ReadZlinkSource([CallerFilePath] string file = "")
    {
        string sourceRoot = Path.Combine(BindingRoot(file), "src", "Zlink");
        var chunks = new List<string>();
        foreach (string path in Directory.EnumerateFiles(sourceRoot, "*.cs",
                     SearchOption.AllDirectories))
        {
            if (path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal))
                continue;
            if (path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal))
                continue;
            chunks.Add(File.ReadAllText(path));
        }
        return string.Join('\n', chunks);
    }

    private static string ReadSampleAndPerfSource([CallerFilePath] string file = "")
    {
        string bindingRoot = BindingRoot(file);
        string[] roots =
        {
            Path.Combine(bindingRoot, "samples"),
            Path.Combine(bindingRoot, "perf")
        };
        var chunks = new List<string>();
        foreach (string root in roots)
        {
            foreach (string path in Directory.EnumerateFiles(root, "*.cs",
                         SearchOption.AllDirectories))
            {
                if (path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                        StringComparison.Ordinal))
                    continue;
                if (path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                        StringComparison.Ordinal))
                    continue;
                chunks.Add(File.ReadAllText(path));
            }
        }
        return string.Join('\n', chunks);
    }

    private static string BindingRoot([CallerFilePath] string file = "")
    {
        string repoRoot = Path.GetFullPath(Path.Combine(
            Path.GetDirectoryName(file)!,
            "..", "..", "..", ".."));
        return Path.Combine(repoRoot, "bindings", "dotnet");
    }

    private static HashSet<string> ReadPublicTypeDeclarations(string root)
    {
        var declarations = new HashSet<string>(StringComparer.Ordinal);
        foreach (string path in Directory.EnumerateFiles(root, "*.cs",
                     SearchOption.AllDirectories))
        {
            string source = File.ReadAllText(path);
            foreach (var declaration in ReadPublicTypeDeclarations(source, path))
                declarations.Add(declaration.TypeName);
        }
        return declarations;
    }

    private static string ReadSourceTree(string root)
    {
        var chunks = new List<string>();
        foreach (string path in Directory.EnumerateFiles(root, "*.cs",
                     SearchOption.AllDirectories))
            chunks.Add(File.ReadAllText(path));
        return string.Join('\n', chunks);
    }

    private static List<(string TypeName, string Path, bool IsPartial)>
        ReadPublicTypeDeclarationLocations(
        string root)
    {
        var declarations = new List<(string TypeName, string Path, bool IsPartial)>();
        foreach (string path in Directory.EnumerateFiles(root, "*.cs",
                     SearchOption.AllDirectories))
        {
            string source = File.ReadAllText(path);
            declarations.AddRange(ReadPublicTypeDeclarations(source, path));
        }
        return declarations;
    }

    private static IEnumerable<(string TypeName, string Path, bool IsPartial)>
        ReadPublicTypeDeclarations(string source, string path)
    {
        foreach (Match match in PublicNamedTypeDeclaration.Matches(source))
            yield return (match.Groups["Name"].Value, path,
                HasModifier(match.Groups["Modifiers"].Value, "partial"));

        foreach (Match match in PublicDelegateDeclaration.Matches(source))
            yield return (match.Groups["Name"].Value, path, false);
    }

    private static bool HasModifier(string modifiers, string expected)
    {
        foreach (string modifier in modifiers.Split((char[]?)null,
                     StringSplitOptions.RemoveEmptyEntries))
        {
            if (string.Equals(modifier, expected, StringComparison.Ordinal))
                return true;
        }
        return false;
    }

    private static List<string> FindRuntimePublicDeclarationViolations(
        HashSet<string> contractTypes,
        IEnumerable<(string TypeName, string Path, bool IsPartial)> runtimeTypes)
    {
        var violations = new List<string>();
        foreach (var runtimeType in runtimeTypes)
        {
            if (!contractTypes.Contains(runtimeType.TypeName)
                || !runtimeType.IsPartial)
                violations.Add($"{runtimeType.Path}:{runtimeType.TypeName}");
        }
        return violations;
    }
}
