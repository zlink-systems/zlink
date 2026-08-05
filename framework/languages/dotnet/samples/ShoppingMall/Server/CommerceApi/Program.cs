using Microsoft.Extensions.Configuration;

using System.Net.Http.Json;
using ShoppingMall.Server.CommerceApi.Application.OrderWorkflow;
using ShoppingMall.Server.CommerceApi.Infrastructure.ZLink;
using ShoppingMall.Server.CommerceApi.Ports.Outbound;
using ShoppingMall.Server.Configuration;
using ShoppingMall.Server.Shared.Domain;
using ShoppingMall.Server.Shared.Ports.Outbound;
using ShoppingMall.Server.Shared.Store;
using ShoppingMall.Shared.Contracts;
using Systems.Zlink;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace ShoppingMall.Server.CommerceApi;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = SampleTopology.LoadApi(args);
        var topology = configuration.Topology;
        var instance = topology.ForInstance(configuration.InstanceId);
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            configuration.LogDirectory,
            instance.InstanceId);

        builder.WebHost.UseUrls(instance.HttpUrl);
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton(new RedisCommerceStores(topology));
        builder.Services.AddSingleton<IOrderReadModelStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>());
        builder.Services.AddSingleton<ICommerceStateStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>());
        builder.Services.AddSingleton<IOrderWorkflowRouter, ZLinkOrderWorkflowRouter>();
        builder.Services.AddSingleton<OrderStartPreparation>();
        builder.Services.AddSingleton<StartOrderUseCase>();
        builder.Services.AddSingleton<PrepareInventoryReservedOrderUseCase>();
        builder.Services.AddSingleton<GetOrderStateUseCase>();

        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(Program));
            var mesh = options.AddRouteMesh(SampleNames.MeshName)
                .Listen(instance.MeshEndpoint)
                .SetRoutingIdPrefix("commerce-api");
            mesh.Objects().Client();
            mesh.Channel(SampleNames.OrderProjectionChannel).Client();
        });

        var app = builder.Build();
        app.Use(async (context, next) =>
        {
            try
            {
                await next(context);
            }
            catch (Exception error)
            {
                app.Services.GetRequiredService<ILoggerFactory>()
                    .CreateLogger("ShoppingMall.Server.CommerceApi")
                    .LogError(
                        error,
                        "shoppingmall api handler failed: endpoint={Endpoint} error={Error}",
                        context.Request.Path.Value,
                        error.Message);
                throw;
            }
        });
        app.Services.GetRequiredService<RedisCommerceStores>().SeedDefaults();

        app.MapGet("/health", () => Results.Ok(new { ready = true, instance = instance.InstanceId }));
        app.MapPost("/orders/start", async (
            StartOrderReq request,
            StartOrderUseCase useCase,
            CancellationToken cancellationToken) =>
        {
            var response = await useCase.ExecuteAsync(request, cancellationToken);
            return Results.Ok(response);
        });
        app.MapGet("/orders/{orderId}", async (
            string orderId,
            GetOrderStateUseCase useCase,
            CancellationToken cancellationToken) =>
        {
            var response = await useCase.ExecuteAsync(new GetOrderStateReq(orderId), cancellationToken);
            return Results.Ok(response);
        });
        app.MapPost("/self-check/projection/{orderId}/delete", async (
            string orderId,
            IOrderReadModelStore readModels,
            CancellationToken cancellationToken) =>
        {
            await readModels.DeleteAsync(orderId, cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/self-check/projection/{orderId}/rebuild", async (
            string orderId,
            IOrderWorkflowRouter workflows,
            ILoggerFactory loggerFactory,
            CancellationToken cancellationToken) =>
        {
            var state = await workflows.RebuildProjectionAsync(
                new RebuildOrderProjectionReq(orderId),
                cancellationToken);
            loggerFactory.CreateLogger("ShoppingMall.Server.CommerceApi")
                .LogInformation(
                    "shoppingmall projection: rebuilt order={OrderId} status={Status}",
                    state.OrderId,
                    state.Status);
            return Results.Ok(new RebuildOrderProjectionRes(state));
        });
        app.MapPost("/self-check/idempotency/pending", async (
            PendingMappingHttpReq request,
            ICommerceStateStore commerce,
            CancellationToken cancellationToken) =>
        {
            await commerce.CreatePendingMappingAsync(
                request.IdempotencyKey,
                request.OrderId,
                cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/self-check/workflow/inventory-reserved", async (
            StartOrderReq request,
            PrepareInventoryReservedOrderUseCase useCase,
            CancellationToken cancellationToken) =>
        {
            return Results.Ok(await useCase.ExecuteAsync(request, cancellationToken));
        });
        app.MapPost("/self-check/workflow/{orderId}/continue", async (
            string orderId,
            IOrderWorkflowRouter workflows,
            CancellationToken cancellationToken) =>
        {
            var state = await workflows.ContinueAsync(new ContinueOrderWorkflowReq(orderId), cancellationToken);
            return Results.Ok(new ContinueOrderWorkflowRes(state));
        });
        app.MapPost("/self-check/assert", async (
            ServerAssertionReq request,
            RedisCommerceStores stores,
            ILoggerFactory loggerFactory,
            CancellationToken cancellationToken) =>
        {
            var evidence = await stores.EvidenceAsync(
                [
                    request.SuccessfulOrderId,
                    request.PendingRecoveredOrderId,
                    request.ConcurrentOrderId,
                    request.ResumedOrderId,
                    request.InventoryFailureOrderId,
                    request.PaymentFailureOrderId,
                    request.ScaleOutOrderId
                ],
                cancellationToken);
            var lines = evidence.EventsByOrder
                .Select(item => $"{item.Key}:{string.Join(">", item.Value)}")
                .Append($"paymentFailures={evidence.PaymentFailureCount}")
                .Append($"releasedReservations={evidence.ReleasedReservationCount}")
                .Append($"startedIdempotency={evidence.StartedIdempotencyCount}")
                .ToArray();
            var passed =
                HasSequence(evidence, request.SuccessfulOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservedEvent),
                    nameof(PaymentAuthorizedEvent),
                    nameof(OrderConfirmedEvent))
                && HasPrefix(evidence, request.PendingRecoveredOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservedEvent),
                    nameof(PaymentAuthorizedEvent),
                    nameof(OrderConfirmedEvent))
                && HasSequence(evidence, request.ConcurrentOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservedEvent),
                    nameof(PaymentAuthorizedEvent),
                    nameof(OrderConfirmedEvent))
                && HasSequence(evidence, request.ResumedOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservedEvent),
                    nameof(PaymentAuthorizedEvent),
                    nameof(OrderConfirmedEvent))
                && HasSequence(evidence, request.InventoryFailureOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservationFailedEvent),
                    nameof(OrderFailedEvent))
                && HasSequence(evidence, request.PaymentFailureOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservedEvent),
                    nameof(PaymentFailedEvent),
                    nameof(InventoryReleasedEvent),
                    nameof(OrderFailedEvent))
                && HasSequence(evidence, request.ScaleOutOrderId,
                    nameof(OrderStartedEvent),
                    nameof(InventoryReservedEvent),
                    nameof(PaymentAuthorizedEvent),
                    nameof(OrderConfirmedEvent))
                && evidence.ReleasedReservationCount >= 1
                && evidence.PaymentFailureCount >= 1
                && evidence.StartedIdempotencyCount == 7;
            loggerFactory.CreateLogger("ShoppingMall.Server.CommerceApi")
                .LogInformation("shoppingmall evidence: {Evidence}", string.Join("; ", lines));
            return Results.Ok(new ServerAssertionRes(passed, lines));
        });

        await app.RunAsync();
    }

    private static bool HasSequence(
        StoreEvidence evidence,
        string orderId,
        params string[] expected)
    {
        return evidence.EventsByOrder.TryGetValue(orderId, out var actual)
               && actual.SequenceEqual(expected);
    }

    private static bool HasPrefix(
        StoreEvidence evidence,
        string orderId,
        params string[] expected)
    {
        return evidence.EventsByOrder.TryGetValue(orderId, out var actual)
               && actual.Length >= expected.Length
               && actual.Take(expected.Length).SequenceEqual(expected);
    }
}

internal sealed record PendingMappingHttpReq(
    string IdempotencyKey,
    string OrderId);

internal sealed record ServerAssertionReq(
    string SuccessfulOrderId,
    string PendingRecoveredOrderId,
    string ConcurrentOrderId,
    string ResumedOrderId,
    string InventoryFailureOrderId,
    string PaymentFailureOrderId,
    string ScaleOutOrderId);

internal sealed record ServerAssertionRes(
    bool Passed,
    string[] Evidence);
