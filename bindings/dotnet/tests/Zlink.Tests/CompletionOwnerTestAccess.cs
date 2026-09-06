using System.Collections;
using System.Reflection;
using System.Runtime.ExceptionServices;

namespace Systems.Zlink.Tests;

// Test-only access keeps completion injection and submit/publication barriers
// out of the public API and the production native-call path.
internal static class CompletionOwnerTestAccess
{
    private const BindingFlags Instance = BindingFlags.Instance
        | BindingFlags.Public | BindingFlags.NonPublic;

    internal static Type RuntimeType(string name) =>
        typeof(Message).Assembly.GetType(name, throwOnError: true)!;

    internal static object Create(Type type, params object?[] arguments) =>
        Activator.CreateInstance(type, Instance, null, arguments, null)!;

    internal static object? Invoke(object target, string method,
        params object?[] arguments)
    {
        try
        {
            return target.GetType().GetMethod(method, Instance)!
                .Invoke(target, arguments);
        }
        catch (TargetInvocationException exception)
            when (exception.InnerException is not null)
        {
            ExceptionDispatchInfo.Capture(exception.InnerException).Throw();
            throw;
        }
    }

    internal static object Property(object target, string name) =>
        target.GetType().GetProperty(name, Instance)!.GetValue(target)!;

    internal static object Field(object target, string name) =>
        target.GetType().GetField(name, Instance)!.GetValue(target)!;

    internal static void SetField(object target, string name, object value)
    {
        FieldInfo field = target.GetType().GetField(name, Instance)!;
        field.SetValue(target, field.FieldType.IsEnum
            ? Enum.ToObject(field.FieldType, value) : value);
    }

    internal static object Owner(ISocket socket)
    {
        object kernel = RuntimeType("Systems.Zlink.SocketBase")
            .GetProperty("Kernel", Instance)!.GetValue(socket)!;
        return Property(kernel, "Completion");
    }

    internal static object[] Entries(object owner)
    {
        lock (Field(owner, "_sync"))
            return ((IDictionary)Field(owner, "_entries")).Values
                .Cast<object>().ToArray();
    }
}
