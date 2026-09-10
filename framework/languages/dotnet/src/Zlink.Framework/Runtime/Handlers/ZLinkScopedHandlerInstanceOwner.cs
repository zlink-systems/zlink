using System.Collections.Concurrent;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.CompilerServices;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkScopedHandlerInstanceOwner(IServiceProvider services) : IAsyncDisposable
{
    // Microsoft DI shares its service-availability catalog across scopes. Keep
    // constructor selection with that catalog, never with the handler type alone:
    // different hosts can satisfy different constructors of the same handler.
    private static readonly ConditionalWeakTable<object,
        ConcurrentDictionary<Type, Func<IServiceProvider, object>>> Factories = new();

    private readonly (IServiceProviderIsService? Services,
        ConcurrentDictionary<Type, Func<IServiceProvider, object>> Factories) _activation =
        GetActivationFactories(services);
    private readonly Dictionary<Type, object> _fallbackInstances = new();
    private readonly ZLinkStateLane _lane = new();
    private bool _disposed;
    private Task? _disposeTask;

    internal IServiceProvider Services { get; } = services;

    internal static void Prepare(IServiceProvider services, IEnumerable<Type> handlerTypes)
    {
        var activation = GetActivationFactories(services);
        foreach (var handlerType in handlerTypes)
            activation.Factories.GetOrAdd(handlerType,
                static (type, available) => Compile(type, available), activation.Services);
    }

    internal void Prepare(Type handlerType) => GetFactory(handlerType);

    public THandler Resolve<THandler>()
        where THandler : class
    {
        return (THandler)Resolve(typeof(THandler));
    }

    public object Resolve(Type handlerType)
    {
        ArgumentNullException.ThrowIfNull(handlerType);

        return AwaitStateLane(_lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            if (_fallbackInstances.TryGetValue(handlerType, out var existing)) return existing;

            var created = GetFactory(handlerType)(Services);
            _fallbackInstances.Add(handlerType, created);
            return created;
        }));
    }

    private Func<IServiceProvider, object> GetFactory(Type handlerType) =>
        _activation.Factories.GetOrAdd(handlerType,
            static (type, available) => Compile(type, available), _activation.Services);

    private static (IServiceProviderIsService? Services,
        ConcurrentDictionary<Type, Func<IServiceProvider, object>> Factories)
        GetActivationFactories(IServiceProvider services)
    {
        var available = services.GetService<IServiceProviderIsService>();
        // Without a catalog, CreateFactory's selection depends only on the type.
        return (available, Factories.GetValue(
            (object?)available ?? typeof(ActivatorUtilities), static _ => new()));
    }

    private static Func<IServiceProvider, object> Compile(
        Type handlerType, IServiceProviderIsService? available)
    {
        try
        {
            return CompileFactory(handlerType, available);
        }
        catch (InvalidOperationException exception)
        {
            // Discovery also includes handlers that may never run. Preparing a
            // plan must preserve their activation error at Resolve, without
            // constructing a handler or its scoped dependencies during startup.
            var failure = System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(exception);
            return _ =>
            {
                failure.Throw();
                return null!;
            };
        }
    }

    private static Func<IServiceProvider, object> CompileFactory(
        Type handlerType, IServiceProviderIsService? available)
    {
        if (handlerType.IsAbstract)
            throw new InvalidOperationException("Instances of abstract classes cannot be created.");

        ConstructorInfo? selected = null;
        var bestLength = -1;
        var multiple = false;
        var seenPreferred = false;
        if (available is not null)
            foreach (var constructor in handlerType.GetConstructors())
            {
                var parameters = constructor.GetParameters();
                var length = parameters.Length;
                foreach (var parameter in parameters)
                    if (!IsService(available, parameter) && !parameter.HasDefaultValue)
                    {
                        length = -1;
                        break;
                    }

                var preferred = constructor.IsDefined(typeof(ActivatorUtilitiesConstructorAttribute), false);
                if (preferred)
                {
                    if (seenPreferred)
                        throw new InvalidOperationException(
                            "Multiple constructors were marked with ActivatorUtilitiesConstructorAttribute.");
                    if (length < 0)
                        throw new InvalidOperationException(
                            "Constructor marked with ActivatorUtilitiesConstructorAttribute does not accept all given argument types.");
                }

                // Preserve the pinned DI 8 constructor-selection semantics,
                // including equal-length ambiguity and preferred constructors.
                if (preferred || length > bestLength)
                {
                    selected = constructor;
                    bestLength = length;
                    multiple = false;
                }
                else if (length == bestLength)
                    multiple = true;
                seenPreferred |= preferred;
            }

        if (bestLength < 0)
        {
            var factory = ActivatorUtilities.CreateFactory(handlerType, Type.EmptyTypes);
            return provider => factory(provider, null);
        }
        if (multiple)
            throw new InvalidOperationException(
                $"Multiple constructors for type '{handlerType}' were found with length {bestLength}.");

        var services = Expression.Parameter(typeof(IServiceProvider), "services");
        var arguments = selected!.GetParameters().Select(parameter =>
        {
            var key = parameter.GetCustomAttribute<FromKeyedServicesAttribute>()?.Key;
            Expression resolve = key is null
                ? Expression.Call(services, nameof(IServiceProvider.GetService), Type.EmptyTypes,
                    Expression.Constant(parameter.ParameterType, typeof(Type)))
                : Expression.Call(typeof(ZLinkScopedHandlerInstanceOwner), nameof(GetKeyedService),
                    Type.EmptyTypes, services, Expression.Constant(parameter.ParameterType, typeof(Type)),
                    Expression.Constant(key, typeof(object)));
            Expression missing = parameter.HasDefaultValue
                ? Expression.Convert(parameter.DefaultValue is null
                        ? Expression.Default(parameter.ParameterType)
                        : Expression.Convert(Expression.Constant(parameter.DefaultValue), parameter.ParameterType),
                    typeof(object))
                : Expression.Throw(Expression.New(
                        typeof(InvalidOperationException).GetConstructor([typeof(string)])!,
                        Expression.Constant(
                            $"Unable to resolve service for type '{parameter.ParameterType}' while attempting to activate '{handlerType}'.")),
                    typeof(object));
            return Expression.Convert(Expression.Coalesce(resolve, missing), parameter.ParameterType);
        });
        return Expression.Lambda<Func<IServiceProvider, object>>(
                Expression.New(selected, arguments), services)
            .Compile();
    }

    private static bool IsService(IServiceProviderIsService available, ParameterInfo parameter)
    {
        var key = parameter.GetCustomAttribute<FromKeyedServicesAttribute>()?.Key;
        if (key is null) return available.IsService(parameter.ParameterType);
        if (available is IServiceProviderIsKeyedService keyed)
            return keyed.IsKeyedService(parameter.ParameterType, key);
        throw new InvalidOperationException("This service provider doesn't support keyed services.");
    }

    private static object? GetKeyedService(IServiceProvider services, Type type, object key) =>
        services is IKeyedServiceProvider keyed
            ? keyed.GetKeyedService(type, key)
            : throw new InvalidOperationException("This service provider doesn't support keyed services.");

    public ValueTask DisposeAsync()
    {
        var result = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposeTask is null)
            {
                _disposed = true;
                var instances = _fallbackInstances.Values.Reverse().ToArray();
                _fallbackInstances.Clear();
                var completion = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = completion.Task;
                return (Task: _disposeTask, Completion: completion, Instances: instances);
            }
            return (Task: _disposeTask, Completion: (TaskCompletionSource?)null, Instances: (object[]?)null);
        }));
        if (result.Completion is not null)
            StartDisposeCore(result.Completion, result.Instances!);
        return new ValueTask(result.Task);
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void StartDisposeCore(TaskCompletionSource completion, object[] instances)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            _ = DisposeCoreAsync(completion, instances);
            return;
        }

        using (ExecutionContext.SuppressFlow())
            _ = DisposeCoreAsync(completion, instances);
    }

    private static async Task DisposeCoreAsync(TaskCompletionSource completion, object[] instances)
    {
        try
        {
            List<Exception>? failures = null;
            foreach (var instance in instances)
                try
                {
                    if (instance is IAsyncDisposable asyncDisposable)
                        await asyncDisposable.DisposeAsync().ConfigureAwait(false);
                    else if (instance is IDisposable disposable)
                        disposable.Dispose();
                }
                catch (Exception exception)
                {
                    (failures ??= []).Add(exception);
                }

            if (failures is { Count: 1 })
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
            if (failures is { Count: > 1 }) throw new AggregateException(failures);
            completion.TrySetResult();
        }
        catch (OperationCanceledException exception)
        {
            completion.TrySetCanceled(exception.CancellationToken);
        }
        catch (Exception exception)
        {
            completion.TrySetException(exception);
        }
    }
}
