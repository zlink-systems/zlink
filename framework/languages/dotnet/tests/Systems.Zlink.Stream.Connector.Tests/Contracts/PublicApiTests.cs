using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Contracts.Calls;
using System.Reflection;
using Xunit;

public sealed partial class StreamConnectorTests
{
    private static readonly string[] PublicTypeDeclarationTokens =
    [
        "public interface ",
        "public class ",
        "public sealed class ",
        "public abstract class ",
        "public record ",
        "public sealed record ",
        "public enum ",
        "public struct ",
        "public readonly struct ",
        "public delegate ",
        "public static class "
    ];

    private static readonly string[] InternalTypeDeclarationTokens =
    [
        "internal interface ",
        "internal class ",
        "internal sealed class ",
        "internal abstract class ",
        "internal record ",
        "internal sealed record ",
        "internal enum ",
        "internal struct ",
        "internal readonly struct ",
        "internal delegate ",
        "internal static class "
    ];

    [Fact]
    public void ConnectorImplementationIsHiddenBehindPublicInterface()
    {
        var assembly = typeof(IZlinkStreamConnector).Assembly;
        var implementation = assembly.GetType(
            "Systems.Zlink.Stream.Connector.Runtime.ZlinkStreamConnector",
            true)!;

        Assert.False(implementation.IsPublic);
        Assert.True(typeof(IZlinkStreamConnector).IsAssignableFrom(implementation));

        var create = typeof(ZlinkStreamConnectorFactory).GetMethod(nameof(ZlinkStreamConnectorFactory.Create))!;
        Assert.Equal(typeof(IZlinkStreamConnector), create.ReturnType);
    }

    [Fact]
    public void ConnectorSendAndRequestExposeInterfaces()
    {
        var send = typeof(IZlinkStreamConnector).GetMethod(nameof(IZlinkStreamConnector.Send))!;
        var request = typeof(IZlinkStreamConnector).GetMethod(nameof(IZlinkStreamConnector.Request))!;

        Assert.Equal(typeof(IZlinkStreamSendCall), send.ReturnType);
        Assert.Equal(typeof(IZlinkStreamRequestCall), request.ReturnType);
    }

    [Fact]
    public void ReceivedMessageDroppedAppendsWithoutRenumberingExistingErrorCodes()
    {
        Assert.Equal(14, (int)ZlinkStreamErrorCode.RemoteError);
        Assert.Equal(15, (int)ZlinkStreamErrorCode.ReceivedMessageDropped);
    }

    [Fact]
    public void ConnectorCallInterfacesMatchTheFrozenSurface()
    {
        AssertMethod(
            typeof(IZlinkStreamLifecycleCall),
            nameof(IZlinkStreamLifecycleCall.Async),
            typeof(ValueTask),
            (typeof(CancellationToken), true));

        AssertMethod(
            typeof(IZlinkStreamSendCall),
            nameof(IZlinkStreamSendCall.PacketName),
            typeof(IZlinkStreamSendCall),
            (typeof(string), false));
        Assert.Equal(2, typeof(IZlinkStreamSendCall).GetMethods().Count(
            static method => method.Name == nameof(IZlinkStreamSendCall.Metadata)));
        AssertMethod(
            typeof(IZlinkStreamSendCall),
            nameof(IZlinkStreamSendCall.Compress),
            typeof(IZlinkStreamSendCall));
        AssertMethod(
            typeof(IZlinkStreamSendCall),
            nameof(IZlinkStreamSendCall.Async),
            typeof(ValueTask),
            (typeof(CancellationToken), true));

        Assert.Equal(2, typeof(IZlinkStreamRequestCall).GetMethods().Count(
            static method => method.Name == nameof(IZlinkStreamRequestCall.Metadata)));
        Assert.Equal(2, typeof(IZlinkStreamRequestCall).GetMethods().Count(
            static method => method.Name == nameof(IZlinkStreamRequestCall.Submit)));
        AssertMethod(
            typeof(IZlinkStreamRequestCall),
            nameof(IZlinkStreamRequestCall.Async),
            typeof(ValueTask<ZlinkStreamEncodedPayload>),
            (typeof(CancellationToken), true));
        AssertMethod(
            typeof(IZlinkStreamWaitCall),
            nameof(IZlinkStreamWaitCall.Async),
            typeof(ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>),
            (typeof(CancellationToken), true));
        AssertMethod(
            typeof(IZlinkStreamExpectNoneCall),
            nameof(IZlinkStreamExpectNoneCall.Within),
            typeof(IZlinkStreamExpectNoneCall),
            (typeof(TimeSpan), false));
        AssertMethod(
            typeof(IZlinkStreamExpectNoneCall),
            nameof(IZlinkStreamExpectNoneCall.Async),
            typeof(ValueTask),
            (typeof(CancellationToken), true));
        AssertMethod(
            typeof(IZlinkStreamSequenceCall),
            nameof(IZlinkStreamSequenceCall.Expect),
            typeof(IZlinkStreamSequenceCall),
            (typeof(Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>), false));
        AssertMethod(
            typeof(IZlinkStreamSequenceCall),
            nameof(IZlinkStreamSequenceCall.Async),
            typeof(ValueTask<IReadOnlyList<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>>),
            (typeof(CancellationToken), true));

        Assert.Empty(typeof(ZlinkStreamTypedSendBuilder).GetConstructors());
        Assert.Empty(typeof(ZlinkStreamTypedRequestBuilder).GetConstructors());
        Assert.Empty(typeof(ZlinkStreamTypedWaitBuilder<>).GetConstructors());
        Assert.Empty(typeof(ZlinkStreamTypedExpectNoneBuilder<>).GetConstructors());
        Assert.Empty(typeof(ZlinkStreamTypedSequenceBuilder<>).GetConstructors());
        AssertMethod(
            typeof(ZlinkStreamTypedSendBuilder),
            nameof(ZlinkStreamTypedSendBuilder.Async),
            typeof(ValueTask),
            (typeof(CancellationToken), true));
        Assert.Contains(
            typeof(ZlinkStreamTypedRequestBuilder).GetMethods(),
            static method => method.Name == nameof(ZlinkStreamTypedRequestBuilder.Async)
                             && method.IsGenericMethodDefinition
                             && method.GetParameters() is [{ ParameterType: var token, HasDefaultValue: true }]
                             && token == typeof(CancellationToken));

        Assert.Equal(
            typeof(IZlinkStreamExpectNoneCall),
            typeof(IZlinkStreamConnector).GetMethod(nameof(IZlinkStreamConnector.ExpectNone))!.ReturnType);
        Assert.Equal(
            typeof(IZlinkStreamSequenceCall),
            typeof(IZlinkStreamConnector).GetMethod(nameof(IZlinkStreamConnector.WaitForSequence))!.ReturnType);
        AssertMethod(
            typeof(ZlinkStreamAssert),
            nameof(ZlinkStreamAssert.Ensure),
            typeof(void),
            (typeof(bool), false),
            (typeof(string), false));
        AssertMethod(
            typeof(ZlinkStreamAssert),
            nameof(ZlinkStreamAssert.ExpectFailureAsync),
            typeof(ValueTask<ZlinkStreamError>),
            (typeof(Func<CancellationToken, ValueTask>), false),
            (typeof(string), true));
        AssertMethod(
            typeof(ZlinkStreamAssert),
            nameof(ZlinkStreamAssert.ExpectTimeoutAsync),
            typeof(ValueTask),
            (typeof(Func<CancellationToken, ValueTask>), false));
    }

    [Fact]
    public void DisconnectEventCarriesTheFrozenCloseReasonContract()
    {
        var eventInfo = typeof(IZlinkStreamConnector).GetEvent(nameof(IZlinkStreamConnector.Disconnected));
        Assert.NotNull(eventInfo);
        Assert.Equal(
            typeof(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>),
            eventInfo!.EventHandlerType);
        Assert.Equal(
            new[]
            {
                "ClientClose", "HeartbeatTimeout", "IdleTimeout", "ProtocolError", "ServerDrain", "TransportError"
            },
            Enum.GetNames<ZlinkStreamCloseReason>().Order(StringComparer.Ordinal).ToArray());
    }

    [Fact]
    public void ConnectorOptionsMatchTheFrozenDefaults()
    {
        var endpoint = new Uri("tcp://127.0.0.1:1");
        var options = new ZlinkStreamConnectorOptions { Endpoint = endpoint };

        Assert.Equal(endpoint, options.Endpoint);
        Assert.Null(options.Transport);
        Assert.Equal(TimeSpan.FromSeconds(5), options.ConnectTimeout);
        Assert.Equal(TimeSpan.FromSeconds(30), options.RequestTimeout);
        Assert.Equal(TimeSpan.FromSeconds(5), options.WaitTimeout);
        Assert.Equal(64 * 1024, options.MaxSendPayloadSize);
        Assert.Equal(64 * 1024, options.MaxReceivePayloadSize);
        Assert.Equal(1024, options.MaxReceivedMessages);
        Assert.Equal(1024, options.MaxPendingDispatchCallbacks);
        Assert.Equal(1024, options.MaxInboundObserverNotifications);
        Assert.Equal(0, options.MaxInboundObserverPayloadPreviewBytes);
        Assert.False(options.SkipServerCertificateValidation);
        Assert.Equal(ZlinkStreamDispatchMode.Manual, options.DispatchMode);
        Assert.Equal(ZlinkStreamCompression.Lz4, options.Compression);
        Assert.Null(options.CompressionCodec);
        Assert.IsType<ZlinkStreamPacketNameResolver>(options.NameResolver);
        Assert.Null(options.PayloadCodec);

        Assert.True(options.Heartbeat.Enabled);
        Assert.Equal(TimeSpan.FromSeconds(1), options.Heartbeat.Interval);
        Assert.Equal(TimeSpan.FromSeconds(5), options.Heartbeat.Timeout);
        Assert.True(options.Reconnect.Enabled);
        Assert.Equal(TimeSpan.FromMilliseconds(250), options.Reconnect.InitialDelay);
        Assert.Equal(TimeSpan.FromSeconds(5), options.Reconnect.MaxDelay);
        Assert.Equal(2.0, options.Reconnect.BackoffFactor);
        Assert.Equal(3, options.Reconnect.MaxAttempts);

        var endpointProperty = typeof(ZlinkStreamConnectorOptions).GetProperty(
            nameof(ZlinkStreamConnectorOptions.Endpoint))!;
        Assert.Contains(
            endpointProperty.GetCustomAttributesData(),
            static attribute => attribute.AttributeType.FullName ==
                                "System.Runtime.CompilerServices.RequiredMemberAttribute");
    }

    [Fact]
    public void ConnectorPublicTypeSourceFiles_LiveUnderContracts()
    {
        var sourceRoot = GetConnectorSourceRoot();
        var violations = Directory
            .EnumerateFiles(sourceRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .Where(path => HasPublicTypeDeclaration(File.ReadAllText(path)))
            .Where(path => !Path.GetRelativePath(sourceRoot, path)
                .StartsWith($"Contracts{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
            .Select(path => Path.GetRelativePath(sourceRoot, path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(violations);
    }

    [Fact]
    public void ConnectorContracts_DoNotContain_InternalTypeDeclarations()
    {
        var contractsRoot = Path.Combine(GetConnectorSourceRoot(), "Contracts");
        var violations = Directory
            .EnumerateFiles(contractsRoot, "*.cs", SearchOption.AllDirectories)
            .Where(path => HasInternalTypeDeclaration(File.ReadAllText(path)))
            .Select(path => Path.GetRelativePath(contractsRoot, path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(violations);
    }

    [Fact]
    public void ConnectorExportedTypes_UseContractsNamespace()
    {
        var violations = typeof(IZlinkStreamConnector).Assembly
            .GetExportedTypes()
            .Where(static type => type.Namespace is null
                                  || (!type.Namespace.Equals("Systems.Zlink.Stream.Connector.Contracts",
                                          StringComparison.Ordinal)
                                      && !type.Namespace.StartsWith("Systems.Zlink.Stream.Connector.Contracts.",
                                          StringComparison.Ordinal)))
            .Select(static type => type.FullName)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(violations);
    }

    [Fact]
    public void ConnectorDefaultCodecFactory_IsNotPublicContract()
    {
        var exportedTypeNames = typeof(IZlinkStreamConnector).Assembly
            .GetExportedTypes()
            .Select(static type => type.Name)
            .ToArray();

        Assert.DoesNotContain("ZlinkStreamDefaultCodecs", exportedTypeNames);
        Assert.DoesNotContain("ZlinkStreamDefaultCodecFactory", exportedTypeNames);
        Assert.Contains("IZlinkStreamCompressionCodec", exportedTypeNames);
    }

    private static string GetConnectorSourceRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var candidate = Path.Combine(
                directory.FullName,
                "src",
                "Systems.Zlink.Stream.Connector");
            if (Directory.Exists(candidate)) return candidate;

            directory = directory.Parent;
        }

        throw new DirectoryNotFoundException("Could not locate Systems.Zlink.Stream.Connector source root.");
    }

    private static bool HasPublicTypeDeclaration(string source)
    {
        return source
            .Split('\n')
            .Select(static line => line.TrimEnd('\r'))
            .Any(static line =>
                PublicTypeDeclarationTokens.Any(token => line.StartsWith(token, StringComparison.Ordinal)));
    }

    private static bool HasInternalTypeDeclaration(string source)
    {
        return source
            .Split('\n')
            .Select(static line => line.TrimEnd('\r'))
            .Any(static line =>
                InternalTypeDeclarationTokens.Any(token => line.StartsWith(token, StringComparison.Ordinal)));
    }

    private static void AssertMethod(
        Type type,
        string name,
        Type returnType,
        params (Type Type, bool HasDefault)[] expectedParameters)
    {
        var method = Assert.Single(type.GetMethods().Where(candidate =>
            candidate.Name == name
            && candidate.GetParameters().Select(static parameter => parameter.ParameterType)
                .SequenceEqual(expectedParameters.Select(static parameter => parameter.Type))));
        Assert.Equal(returnType, method.ReturnType);
        Assert.Equal(
            expectedParameters.Select(static parameter => parameter.HasDefault),
            method.GetParameters().Select(static parameter => parameter.HasDefaultValue));
    }
}
