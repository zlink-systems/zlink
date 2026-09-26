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
    public void request_uses_whole_message_core_api_without_binding_registry()
    {
        string path = Path.Combine(BindingRoot(), "src", "Zlink", "Runtime",
            "Messaging", "CompletionOwner.cs");
        string source = File.ReadAllText(path);

        Assert.Contains("NativeMethods.zlink_request(", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("SelectRouterTarget", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("SendCompletionRegistry", source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void completion_progress_has_no_runtime_background_owner()
    {
        string path = Path.Combine(BindingRoot(), "src", "Zlink", "Runtime",
            "Messaging", "CompletionOwner.cs");
        string source = File.ReadAllText(path);

        Assert.DoesNotContain("new Thread(", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Thread.Sleep", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("new Timer(", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("PeriodicTimer", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Yield()", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Run(", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("RuntimePump", source,
            StringComparison.Ordinal);
        Assert.Contains("EnsurePublicOwner();", source,
            StringComparison.Ordinal);
        Assert.Contains("DrainInline(entry);", source,
            StringComparison.Ordinal);
        Assert.Contains("zlink_completion_recv(_handle,", source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void async_send_retains_payload_only_after_backpressure()
    {
        string path = Path.Combine(BindingRoot(), "src", "Zlink", "Runtime",
            "Messaging", "CompletionOwner.cs");
        string source = File.ReadAllText(path);

        int firstAttempt = source.IndexOf(
            "var attempt = SubmitSend(target, parts, DontWait,",
            StringComparison.Ordinal);
        int retainedSnapshot = source.IndexOf(
            "retained = RequestReplySupport.CloneParts(parts);",
            StringComparison.Ordinal);

        Assert.True(firstAttempt >= 0);
        Assert.True(retainedSnapshot > firstAttempt);
    }

    [Fact]
    public void async_request_retains_payload_only_after_backpressure()
    {
        string path = Path.Combine(BindingRoot(), "src", "Zlink", "Runtime",
            "Messaging", "CompletionOwner.cs");
        string source = File.ReadAllText(path);

        int requestPath = source.IndexOf(
            "internal RequestSubmission RequestAsync",
            StringComparison.Ordinal);
        int firstAttempt = source.IndexOf(
            "var attempt = SubmitRequest(target, parts, timeoutMs, DontWait,",
            requestPath, StringComparison.Ordinal);
        int retainedSnapshot = source.IndexOf(
            "retained = RequestReplySupport.CloneParts(parts);",
            firstAttempt, StringComparison.Ordinal);
        int writableRetry = source.IndexOf("private void RetryRequest()",
            retainedSnapshot, StringComparison.Ordinal);

        Assert.True(requestPath >= 0);
        Assert.True(firstAttempt > requestPath);
        Assert.True(retainedSnapshot > firstAttempt);
        Assert.True(writableRetry > retainedSnapshot);
    }

    [Fact]
    public void shutdown_errno_maps_to_terminated_submit_result()
    {
        string path = Path.Combine(BindingRoot(), "src", "Zlink", "Runtime",
            "Errors", "ZlinkException.Native.cs");
        string source = File.ReadAllText(path);

        Assert.Contains("EshutdownFallback", source,
            StringComparison.Ordinal);
        Assert.Contains("58 or 108 or 10058 or EshutdownFallback", source,
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
    public void perf_measurement_helpers_branch_on_submit_result()
    {
        string path = Path.Combine(BindingRoot(), "perf", "common",
            "Zlink.BindingBench.Common", "PerfSocketIo.cs");
        string source = File.ReadAllText(path);

        Assert.Equal(4, Regex.Matches(source,
            @"public static SendSubmission SendMeasurementAsync\(").Count);
        Assert.DoesNotContain("IsCompleted", source,
            StringComparison.Ordinal);
        Assert.Contains("submission.Result == SubmitResult.Backpressured", source,
            StringComparison.Ordinal);
        Assert.Contains("submission.Admitted", source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void routed_multi_clients_use_public_completion_poller_rounds()
    {
        string sourceRoot = Path.Combine(BindingRoot(), "perf", "multi",
            "Zlink.BindingBench.Multi", "src");
        string[] clients =
        {
            "PerfMultiDealerRouterClient.cs",
            "PerfMultiRouterRouterClient.cs"
        };

        foreach (string client in clients)
        {
            string source = File.ReadAllText(Path.Combine(sourceRoot, client));
            int round = source.IndexOf("int roundStart = 0;",
                StringComparison.Ordinal);
            Assert.True(round >= 0, client);

            int poll = source.IndexOf("PollSocketEvents(", round,
                StringComparison.Ordinal);
            Assert.True(poll > round, client);

            int receiveDrain = source.IndexOf("HandleClientEvent(", poll,
                StringComparison.Ordinal);

            Assert.True(receiveDrain > poll, client);
            Assert.Contains(
                "for (int attempts = 0; attempts < slots.Length; attempts++)",
                source, StringComparison.Ordinal);
            Assert.Contains("if (slot.AdmissionPending)", source,
                StringComparison.Ordinal);
            Assert.Contains(
                "while (Stopwatch.GetTimestamp() < benchDeadlineTicks)", source,
                StringComparison.Ordinal);
            Assert.Contains("submission.Result == SubmitResult.Ok", source,
                StringComparison.Ordinal);
            Assert.Contains(
                "SocketPollIn | PollEventFlags.PollCompletion", source,
                StringComparison.Ordinal);
            Assert.Contains(
                "PollSocketEvents(pollManager, sockets, eventMasks, 0);", source,
                StringComparison.Ordinal);
            Assert.Contains("int completionWaitMs = submittedAny", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("admissionSignal.", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("Math.Min(50, remainingMs)", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("await Task.Yield()", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("SendLoopAsync", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("new Task[slots.Length]", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("WaitingForReply", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("IsCompleted", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("TryCompletePendingAdmission", source,
                StringComparison.Ordinal);
        }

        string signal = File.ReadAllText(Path.Combine(sourceRoot,
            "PerfMultiAdmissionSignal.cs"));
        Assert.Contains("poll(Math.Min(50, timeoutMs))", signal,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay", signal,
            StringComparison.Ordinal);
    }

    [Fact]
    public void message_vector_move_captures_errno_before_dispose()
    {
        string source = File.ReadAllText(Path.Combine(BindingRoot(), "src",
            "Zlink", "Runtime", "Messaging", "Message.NativeVector.cs"));
        int move = source.IndexOf("var rc = NativeMethods.zlink_msg_move(ref msg._msg",
            StringComparison.Ordinal);
        Assert.True(move >= 0);
        int capture = source.IndexOf("var errno = NativeMethods.GetLastPInvokeError();",
            move, StringComparison.Ordinal);
        int dispose = source.IndexOf("msg.Dispose();", move,
            StringComparison.Ordinal);
        Assert.True(capture > move && capture < dispose);
    }

    [Fact]
    public void remaining_multi_async_senders_register_public_completion_owners()
    {
        string sourceRoot = Path.Combine(BindingRoot(), "perf", "multi",
            "Zlink.BindingBench.Multi", "src");

        string dealerDealer = File.ReadAllText(Path.Combine(sourceRoot,
            "PerfMultiDealerDealerClient.cs"));
        Assert.Contains("Array.Fill(completionMasks, PollEventFlags.PollCompletion)",
            dealerDealer, StringComparison.Ordinal);
        Assert.True(dealerDealer.IndexOf(
                "PollSocketEvents(context, pollManager, activeClients,",
                StringComparison.Ordinal)
            < dealerDealer.IndexOf("sendTasks[i] = SendLoopAsync",
                StringComparison.Ordinal));

        string relay = File.ReadAllText(Path.Combine(sourceRoot,
            "PerfMultiRoutedRelayServer.cs"));
        Assert.Contains("SocketPollIn | PollEventFlags.PollCompletion", relay,
            StringComparison.Ordinal);
        Assert.Contains("while (!replySender.Completion.IsCompleted)", relay,
            StringComparison.Ordinal);

        string stream = File.ReadAllText(Path.Combine(sourceRoot,
            "PerfMultiStreamServer.cs"));
        Assert.Contains(
            "completionPoller.Add(server, PollEventFlags.PollCompletion, 0)",
            stream, StringComparison.Ordinal);
        Assert.True(stream.IndexOf(
                "completionPoller.Add(server, PollEventFlags.PollCompletion, 0)",
                StringComparison.Ordinal)
            < stream.IndexOf("Task dispatcher = DispatchSendsAsync",
                StringComparison.Ordinal));
    }

    [Fact]
    public void single_request_perf_waits_only_for_backpressured_admission()
    {
        string path = Path.Combine(BindingRoot(), "perf", "single",
            "Zlink.BindingBench", "src", "PerfReqRep.cs");
        string source = File.ReadAllText(path);

        Assert.Contains("RequestSubmission submission = submit(message);", source,
            StringComparison.Ordinal);
        Assert.Contains("completionPoller.Add(requester,",
            source, StringComparison.Ordinal);
        Assert.Contains("submission.Reply", source,
            StringComparison.Ordinal);
        Assert.Contains("completionPoller.Wait(", source,
            StringComparison.Ordinal);
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
