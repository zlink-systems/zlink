using System;
using System.Linq;
using System.Reflection;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_socket_surface
{
    private static MethodInfo[] PublicInstanceMethods(Type type)
    {
        return type.IsInterface
            ? type.GetInterfaces()
                .Append(type)
                .SelectMany(current => current.GetMethods())
                .ToArray()
            : type.GetMethods(BindingFlags.Instance | BindingFlags.Public);
    }

    private static bool HasPublicInstanceMethod(
        Type type,
        string name,
        params Type[] parameterTypes)
    {
        return PublicInstanceMethods(type).Any(method =>
            method.Name == name
            && method.GetParameters().Select(parameter => parameter.ParameterType)
                .SequenceEqual(parameterTypes));
    }

    private static bool HasPublicSinglePartRoutedSendShortcut()
    {
        return typeof(MessageOperations).GetMethods(
                BindingFlags.Public | BindingFlags.Static)
            .Any(method => method.Name == "SendAsync"
                && method.GetParameters().Select(parameter => parameter.ParameterType)
                    .Take(3)
                    .SequenceEqual([typeof(IRouterSocket), typeof(RoutingId),
                        typeof(Message)]));
    }

    [Fact]
    public void runtime_implementation_types_are_not_public_contract()
    {
        string[] hiddenRuntimeTypeNames =
        {
            "Systems.Zlink.Context",
            "Systems.Zlink.SocketBase"
        };

        string[] exportedTypeNames = typeof(Zlink).Assembly.GetExportedTypes()
            .Select(type => type.FullName!)
            .ToArray();

        foreach (string typeName in hiddenRuntimeTypeNames)
            Assert.DoesNotContain(typeName, exportedTypeNames);

        Assert.DoesNotContain(exportedTypeNames,
            typeName => typeName.StartsWith(
                "Systems.Zlink.Runtime.",
                StringComparison.Ordinal));
    }

    [Fact]
    public void typed_socket_contract_keeps_direction_specific_methods()
    {
        Assert.DoesNotContain(PublicInstanceMethods(typeof(IPubSocket)),
            method => method.Name == "Recv");
        Assert.DoesNotContain(PublicInstanceMethods(typeof(ISubSocket)),
            method => method.Name == "Publish");
        Assert.True(HasPublicInstanceMethod(
            typeof(IPairSocket),
            "Recv",
            typeof(Received),
            typeof(RecvFlags)));
        Assert.False(HasPublicInstanceMethod(
            typeof(IPairSocket), "RecvRetained", typeof(Received),
            typeof(RecvFlags)));
        Assert.False(HasPublicInstanceMethod(
            typeof(IRouterSocket), "RecvRetained", typeof(Received),
            typeof(RecvFlags)));
        Assert.False(HasPublicInstanceMethod(
            typeof(ISubSocket), "SubscribeRetained", typeof(TopicMessage),
            typeof(RecvFlags)));
        Assert.True(HasPublicInstanceMethod(
            typeof(IStreamSocket),
            nameof(IStreamSocket.OnPacket),
            typeof(StreamPacketHandler)));
        Assert.True(HasPublicInstanceMethod(
            typeof(IStreamSocket),
            nameof(IStreamSocket.RecvPart),
            typeof(RoutingId?).MakeByRefType(),
            typeof(Message).MakeByRefType(),
            typeof(bool).MakeByRefType(),
            typeof(RecvFlags)));
    }

    [Fact]
    public void send_exposes_sync_and_async_terminals_while_request_is_async_only()
    {
        MethodInfo dealerSend = PublicInstanceMethods(typeof(IDealerSocket))
            .Single(method => method.Name == nameof(IDealerSocket.Send)
                && method.GetParameters().Length == 0);
        MethodInfo routerSend = PublicInstanceMethods(typeof(IRouterSocket))
            .Single(method => method.Name == nameof(IRouterSocket.Send)
                && method.GetParameters().Select(parameter =>
                        parameter.ParameterType)
                    .SequenceEqual([typeof(RoutingId)]));

        Assert.Equal(typeof(RoutedSendOperation), dealerSend.ReturnType);
        Assert.Equal(typeof(RoutedSendOperation), routerSend.ReturnType);
        Assert.False(typeof(IMessageSocket).IsAssignableFrom(
            typeof(IDealerSocket)));
        Assert.False(typeof(IRoutedMessageSocket).IsAssignableFrom(
            typeof(IRouterSocket)));
        Assert.True(typeof(IReceivingMessageSocket).IsAssignableFrom(
            typeof(IDealerSocket)));
        Assert.True(typeof(IReceivingMessageSocket).IsAssignableFrom(
            typeof(IRouterSocket)));
        Assert.DoesNotContain(PublicInstanceMethods(typeof(IDealerSocket)),
            method => method.Name == nameof(IDealerSocket.Send)
                && method.ReturnType == typeof(SendOperation));
        Assert.DoesNotContain(PublicInstanceMethods(typeof(IRouterSocket)),
            method => method.Name == nameof(IRouterSocket.Send)
                && method.ReturnType == typeof(SendOperation));
        foreach (string runtimeTypeName in new[]
                 {
                     "Systems.Zlink.DealerSocket",
                     "Systems.Zlink.RouterSocket"
                 })
        {
            Type runtimeType = typeof(Zlink).Assembly.GetType(runtimeTypeName)
                ?? throw new InvalidOperationException(
                    $"missing runtime type {runtimeTypeName}");
            Assert.DoesNotContain(PublicInstanceMethods(runtimeType), method =>
                method.Name == "Send"
                && method.ReturnType == typeof(SendOperation));
        }
        Assert.Equal(typeof(RoutedSendOperation),
            PublicInstanceMethods(typeof(IPairSocket)).Single(method =>
                method.Name == nameof(IPairSocket.Send)).ReturnType);
        Assert.Equal(typeof(RoutedSendOperation),
            PublicInstanceMethods(typeof(IStreamSocket)).Single(method =>
                method.Name == nameof(IStreamSocket.Send)).ReturnType);
        Assert.Equal(typeof(SendOperation),
            PublicInstanceMethods(typeof(IStreamSocket)).Single(method =>
                method.Name == nameof(IStreamSocket.TrySend)).ReturnType);
        Assert.Equal(typeof(RoutedSendOperation),
            PublicInstanceMethods(typeof(Received)).Single(method =>
                method.Name == nameof(Received.Send)).ReturnType);
        Assert.True(HasPublicInstanceMethod(typeof(RoutedSendSubmitOperation),
            nameof(RoutedSendSubmitOperation.Submit),
            typeof(SendFlags)));
        Assert.True(HasPublicInstanceMethod(typeof(RoutedSendSubmitOperation),
            nameof(RoutedSendSubmitOperation.Async),
            typeof(System.Threading.CancellationToken)));
        Assert.DoesNotContain(PublicInstanceMethods(
                typeof(RoutedSendSubmitOperation)),
            method => method.Name == "Flags"
                || method.Name == "Submit"
                && method.GetParameters().Length == 0);

        Assert.True(HasPublicInstanceMethod(typeof(RequestSubmitOperation),
            nameof(RequestSubmitOperation.Async),
            typeof(System.Threading.CancellationToken)));
        Assert.DoesNotContain(PublicInstanceMethods(
                typeof(RequestSubmitOperation)),
            method => method.Name is "Submit" or "Flags");
        Assert.DoesNotContain(typeof(Zlink).Assembly.GetExportedTypes(),
            type => type.Name is "RequestCallback"
                or "RequestCallbackSubmitOperation");
        Assert.DoesNotContain(typeof(Zlink).Assembly.GetTypes(),
            type => type.Name == "LegacyRoutedSendOperation");
        Assert.False(HasPublicSinglePartRoutedSendShortcut());
    }

    [Fact]
    public void publish_exposes_only_the_synchronous_submit_terminal()
    {
        MethodInfo publish = PublicInstanceMethods(typeof(IPublisherSocket))
            .Single(method => method.Name == nameof(IPublisherSocket.Publish));
        Assert.Equal(typeof(PublishOperation), publish.ReturnType);

        MethodInfo submit = PublicInstanceMethods(
                typeof(PublishSubmitOperation))
            .Single(method => method.Name == "Submit");
        Assert.Equal(typeof(void), submit.ReturnType);
        Assert.DoesNotContain(PublicInstanceMethods(
                typeof(PublishSubmitOperation)),
            method => method.Name == "Async");

        // The withdrawn send_ready readiness hint has no public surface.
        Assert.DoesNotContain(typeof(Zlink).Assembly.GetExportedTypes()
                .SelectMany(PublicInstanceMethods),
            method => method.Name == "OnSendReady");
        Assert.DoesNotContain(typeof(Zlink).Assembly.GetExportedTypes(),
            type => type.Name is "AsyncSendOperation"
                or "AsyncSendSubmitOperation");
    }
}
