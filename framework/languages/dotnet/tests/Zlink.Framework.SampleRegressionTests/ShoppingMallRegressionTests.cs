using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void ShoppingMall_Uses_One_Physical_Mesh_And_Instance_Spot_Owners()
    {
        var sampleRoot = ResolveSampleRoot("ShoppingMall");
        var hosts = new[]
        {
            Path.Combine(sampleRoot, "Server", "CommerceApi", "Program.cs"),
            Path.Combine(sampleRoot, "Server", "OrderWorkflow", "OrderWorkflowServerHostFactory.cs")
        };

        foreach (var host in hosts)
        {
            var source = File.ReadAllText(host);
            Assert.Equal(1, source.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
            Assert.Contains("AddRouteMesh(SampleNames.MeshName)", source, StringComparison.Ordinal);
            Assert.Contains("AddHandlersFromAssemblyOf", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRequestHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddSendHandler<", source, StringComparison.Ordinal);
        }

        var api = File.ReadAllText(hosts[0]);
        var workflow = File.ReadAllText(hosts[1]);
        Assert.Contains("mesh.Objects().Client()", api, StringComparison.Ordinal);
        Assert.Contains("AddInstanceSpotFactory<OrderWorkflowSpot>", workflow,
            StringComparison.Ordinal);
        Assert.DoesNotContain("OrderWorkflowChannel", api, StringComparison.Ordinal);
        Assert.DoesNotContain("OrderWorkflowChannel", workflow, StringComparison.Ordinal);
    }

    [Fact]
    public void ShoppingMall_Runner_Uses_Isolated_Docker_Redis_And_Redis_Stores()
    {
        var sampleRoot = ResolveSampleRoot("ShoppingMall");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var topology = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleNames.cs"));
        var commerceApi = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CommerceApi", "Program.cs"));
        var commerceWorkflowPorts = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CommerceApi", "Ports",
            "Outbound", "WorkflowPorts.cs"));
        var commerceWorkflowRouter = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CommerceApi",
            "Infrastructure", "ZLink", "ZLinkOrderWorkflowRouter.cs"));
        var workflowHostFactory = File.ReadAllText(Path.Combine(sampleRoot, "Server", "OrderWorkflow",
            "OrderWorkflowServerHostFactory.cs"));
        var workflowService = File.ReadAllText(Path.Combine(sampleRoot, "Server", "OrderWorkflow", "Application",
            "OrderWorkflow", "OrderWorkflowService.cs"));
        var workflowSelfCheck = File.ReadAllText(Path.Combine(sampleRoot, "Server", "OrderWorkflow", "Application",
            "SelfCheck", "OrderWorkflowSelfCheckService.cs"));
        var workflowSpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "OrderWorkflow", "Infrastructure",
            "ZLink", "Spots", "OrderWorkflowSpot", "OrderWorkflowSpot.cs"));
        var startUseCase = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CommerceApi", "Application",
            "OrderWorkflow", "StartOrderUseCase.cs"));
        var messages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts", "Messages.cs"));
        var clientScenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "ShoppingMallClientScenario.cs"));
        var stores = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Shared", "Store", "RedisCommerceStores.cs"));
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.ko.md"));

        Assert.Contains("RUN_ID=\"$(basename \"${RUN_DIR}\")-$$-${RANDOM}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SAMPLE_LOG_DIR=\"${RUN_DIR}/sample-logs\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SHOPPINGMALL_LOG_DIR=\"${SAMPLE_LOG_DIR}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SHOPPINGMALL_REDIS_KEY_PREFIX=\"shoppingmall:dotnet:${RUN_ID}:\"", shellRunner,
            StringComparison.Ordinal);
        Assert.Contains("REDIS_CONTAINER=\"zlink-shoppingmall-dotnet-redis-${RUN_ID}\"", shellRunner,
            StringComparison.Ordinal);
        AssertShellRunnerUsesRedisDockerHelper(
            shellRunner,
            "zlink-shoppingmall-dotnet-redis",
            "SHOPPINGMALL_REDIS_ENDPOINT");
        Assert.DoesNotContain("SHOPPINGMALL_BASE_PORT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_API_A_HTTP_URL:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_API_B_HTTP_URL:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_API_A_ROUTE_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_API_B_ROUTE_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_A_HTTP_URL:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_B_HTTP_URL:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_A_ROUTE_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_B_ROUTE_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_A_SPOT_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTER_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_B_SPOT_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTER_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_REDIS_KEY_PREFIX:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SHOPPINGMALL_LOG_DIR:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("SHOPPINGMALL_STORE_DIR", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("rm -f \"${SHOPPINGMALL_LOG_DIR}\"/*.log", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("SHOPPINGMALL_STARTUP_DELAY_SECONDS", shellRunner, StringComparison.Ordinal);
        Assert.Contains("shoppingmall=completed", shellRunner, StringComparison.Ordinal);
        Assert.Contains("shoppingmall-server-evidence=completed", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", shellRunner, StringComparison.OrdinalIgnoreCase);

        Assert.Contains("$RunId = \"$PID-$([Guid]::NewGuid().ToString('N'))\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$SampleLogDir = Join-Path $RunDir \"sample-logs\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$ports = New-SamplePorts -Count 8 -BasePort 0", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$SHOPPINGMALL_LOG_DIR = $SampleLogDir", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$SHOPPINGMALL_REDIS_KEY_PREFIX = \"shoppingmall:dotnet:${RunId}:\"",
            powershellRunner, StringComparison.Ordinal);
        AssertPowerShellRunnerUsesRedisDockerHelper(powershellRunner, "zlink-shoppingmall-dotnet-redis");
        Assert.DoesNotContain("Set-DefaultEnv", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SHOPPINGMALL_BASE_PORT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SHOPPINGMALL_STORE_DIR", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("SHOPPINGMALL_STARTUP_DELAY_SECONDS", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("Assert-SampleLogContains -LogDirectory $SampleLogDir -Pattern \"shoppingmall=completed\"",
            powershellRunner, StringComparison.Ordinal);
        Assert.Contains("Join-Path $LogDir \"workflow-a.out.log\"", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("Join-Path $LogDir \"workflow-b.out.log\"", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("No workflow instance recorded a shoppingmall order start", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", powershellRunner, StringComparison.OrdinalIgnoreCase);

        Assert.Contains("RedisEndpoint", topology, StringComparison.Ordinal);
        Assert.Contains("RedisKeyPrefix", topology, StringComparison.Ordinal);
        Assert.Contains(
            "OrderWorkflowSpotType = \"shoppingmall.order-workflow\"",
            topology,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Environment.GetEnvironmentVariable", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("SHOPPINGMALL_STORE_DIR", topology, StringComparison.Ordinal);
        Assert.Contains("new RedisCommerceStores(topology)", commerceApi, StringComparison.Ordinal);
        Assert.Contains("new RedisCommerceStores(topology)", workflowHostFactory, StringComparison.Ordinal);
        Assert.Contains("/self-check/workflow/inventory-reserved", commerceApi, StringComparison.Ordinal);
        Assert.DoesNotContain("/self-check/workflow/inventory-reserved", workflowHostFactory,
            StringComparison.Ordinal);
        Assert.Contains("/self-check/workflow/{orderId}/continue", commerceApi, StringComparison.Ordinal);
        Assert.Contains("PrepareInventoryReservedOrderUseCase", commerceApi, StringComparison.Ordinal);
        Assert.Contains("IOrderWorkflowRouter", commerceWorkflowPorts, StringComparison.Ordinal);
        Assert.Contains("PrepareInventoryReservedCheckpointReq", commerceWorkflowRouter, StringComparison.Ordinal);
        Assert.DoesNotContain("ZLinkHttpClient", commerceWorkflowRouter, StringComparison.Ordinal);
        Assert.DoesNotContain("var cart = await commerce.GetCartAsync", commerceApi, StringComparison.Ordinal);
        Assert.DoesNotContain("ReserveIdempotencyAsync", commerceApi, StringComparison.Ordinal);
        Assert.Contains("evidence.StartedIdempotencyCount == 7", commerceApi, StringComparison.Ordinal);
        Assert.DoesNotContain("ForOrderId", commerceApi, StringComparison.Ordinal);
        Assert.DoesNotContain("ownersDiffer", commerceApi, StringComparison.Ordinal);
        Assert.DoesNotContain("OwnerInstanceId", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("OwnerInstanceId", stores, StringComparison.Ordinal);
        Assert.DoesNotContain("ServerAssertionReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("ServerAssertionRes", messages, StringComparison.Ordinal);
        Assert.Contains("record StartOrderRes", messages, StringComparison.Ordinal);
        Assert.Contains("public sealed record StartOrderRes(\n    string OrderId,\n    OrderState State);",
            messages, StringComparison.Ordinal);
        Assert.Contains("internal sealed record ServerAssertionReq", commerceApi, StringComparison.Ordinal);
        Assert.Contains("internal sealed record ServerAssertionReq", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("StartOrderWorkflowToInventoryReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("StartOrderWorkflowToInventoryRes", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("StartOrderWorkflowToInventoryRouteHandler", commerceWorkflowRouter,
            StringComparison.Ordinal);
        Assert.DoesNotContain("StartToInventoryAsync", workflowService, StringComparison.Ordinal);
        Assert.Contains("OrderWorkflowSelfCheckService", workflowHostFactory, StringComparison.Ordinal);
        Assert.Contains("AddInstanceSpotFactory<OrderWorkflowSpot>", workflowHostFactory,
            StringComparison.Ordinal);
        Assert.Contains("PrepareInventoryReservedCheckpointReq", workflowSpot, StringComparison.Ordinal);
        Assert.Contains("ContinueUntilInventoryReservedAsync(command.OrderId, cancellationToken)", workflowSelfCheck,
            StringComparison.Ordinal);
        Assert.Contains("RequestToSpot(orderId, command)", commerceWorkflowRouter,
            StringComparison.Ordinal);
        Assert.Contains(".InstanceSpot(SampleNames.OrderWorkflowSpotType)", commerceWorkflowRouter,
            StringComparison.Ordinal);
        Assert.Contains("CloseIfTerminalAsync", workflowSpot, StringComparison.Ordinal);
        Assert.Contains("Context.CloseAsync(cancellationToken)", workflowSpot, StringComparison.Ordinal);
        Assert.Contains(".InMesh(SampleNames.MeshName)", commerceWorkflowRouter, StringComparison.Ordinal);
        Assert.DoesNotContain("GetOrCreate(", commerceWorkflowRouter, StringComparison.Ordinal);
        Assert.DoesNotContain("IZLinkSpotManager", commerceWorkflowRouter, StringComparison.Ordinal);
        Assert.Contains("IZLinkInstanceSpot", workflowSpot, StringComparison.Ordinal);
        Assert.Contains("IZLinkInstanceSpotContext", workflowSpot, StringComparison.Ordinal);
        Assert.DoesNotContain("OnCreateAsync", workflowSpot, StringComparison.Ordinal);
        Assert.Contains("StartAndContinueAsync", workflowService, StringComparison.Ordinal);
        Assert.DoesNotContain("ContinueAfterStartAsync", workflowSpot, StringComparison.Ordinal);
        Assert.Contains("await workflows.StartAsync(command, cancellationToken)", startUseCase,
            StringComparison.Ordinal);
        Assert.DoesNotContain("ForwardStartAsync", startUseCase, StringComparison.Ordinal);
        Assert.Contains("Task.WhenAll(concurrentA, concurrentB)", clientScenario, StringComparison.Ordinal);
        Assert.Contains("concurrentAResult.OrderId == concurrentBResult.OrderId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("/self-check/workflow/inventory-reserved", clientScenario, StringComparison.Ordinal);
        Assert.Contains($"/self-check/workflow/{{inventoryReserved.OrderId}}/continue", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("resumed.State.ReservationId == $\"reservation-{inventoryReserved.OrderId}\"",
            clientScenario, StringComparison.Ordinal);
        Assert.Contains("resumed.State.PaymentId == $\"payment-{inventoryReserved.OrderId}\"",
            clientScenario, StringComparison.Ordinal);
        Assert.Contains($"/self-check/workflow/{{success.OrderId}}/continue", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("healedByContinue.State.Status == OrderStatuses.Confirmed", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("OrderStatuses.InventoryReserved", clientScenario, StringComparison.Ordinal);
        Assert.Contains("OrderStatuses.PaymentAuthorized", clientScenario, StringComparison.Ordinal);
        Assert.Contains("SaveProjectionFromEventsAsync(stored, cancellationToken)", workflowService,
            StringComparison.Ordinal);
        Assert.Contains("static status => status is OrderStatus.Confirmed or OrderStatus.Failed",
            workflowService, StringComparison.Ordinal);
        Assert.Contains("CancellationToken.None", workflowService, StringComparison.Ordinal);
        Assert.Contains("ContinueWorkflowInBackgroundAsync", workflowService, StringComparison.Ordinal);
        Assert.Contains("catch (OrderStreamVersionConflictException)", workflowService, StringComparison.Ordinal);
        Assert.Contains("throw new OrderStreamVersionConflictException", stores, StringComparison.Ordinal);
        Assert.Contains("if (aggregate.HasProcessedMsg(command.IdempotencyKey))", workflowService,
            StringComparison.Ordinal);
        Assert.Contains("await commerce.MarkIdempotencyStartedAsync(command.IdempotencyKey, cancellationToken);",
            workflowService, StringComparison.Ordinal);
        Assert.Contains("OrderContractMapper.ToContract(\n                await SaveProjectionFromEventsAsync(stored, cancellationToken))", workflowService,
            StringComparison.Ordinal);
        Assert.Contains("OrderStatus.PaymentFailed => await ReleaseInventoryAsync", workflowService,
            StringComparison.Ordinal);
        Assert.Contains("OrderStatus.InventoryReleased => aggregate.FailAfterInventoryRelease", workflowService,
            StringComparison.Ordinal);
        Assert.Contains("public sealed class RedisCommerceStores", stores, StringComparison.Ordinal);
        Assert.Contains("ConnectionMultiplexer.Connect(topology.RedisEndpoint)", stores, StringComparison.Ordinal);
        Assert.Contains("topology.RedisKeyPrefix", stores, StringComparison.Ordinal);
        Assert.Contains("_database.LockTakeAsync", stores, StringComparison.Ordinal);
        Assert.Contains("ReserveInventoryCommand command", stores, StringComparison.Ordinal);
        Assert.Contains("ReleaseInventoryCommand command", stores, StringComparison.Ordinal);
        Assert.Contains("AuthorizePaymentCommand command", stores, StringComparison.Ordinal);
        Assert.Contains("state.Reservations.TryGetValue(command.ReservationId", stores, StringComparison.Ordinal);
        Assert.Contains("InventoryReservationLine", stores, StringComparison.Ordinal);
        Assert.Contains("state.Inventory[line.Sku] = state.Inventory.GetValueOrDefault(line.Sku) + line.Quantity",
            stores, StringComparison.Ordinal);
        Assert.Contains("state.Payments.ContainsKey(command.PaymentId)", stores, StringComparison.Ordinal);
        Assert.Contains("AcquireLockAsync(cancellationToken)", stores, StringComparison.Ordinal);
        Assert.Contains("Task.Delay(10, cancellationToken)", stores, StringComparison.Ordinal);
        Assert.DoesNotContain("File.ReadAllText", stores, StringComparison.Ordinal);
        Assert.DoesNotContain("File.WriteAllText", stores, StringComparison.Ordinal);
        Assert.DoesNotContain("FileStream", stores, StringComparison.Ordinal);

        Assert.Contains("주문 이벤트 스트림, 조회 모델, 장바구니·재고·결제·멱등 상태도 같은 Redis", readme,
            StringComparison.Ordinal);
        Assert.Contains("외부 Redis endpoint 재사용 mode는 제공하지 않는다", readme, StringComparison.Ordinal);
        Assert.Contains("동시에 실행되는 다른 테스트와 섞이지 않는다", readme, StringComparison.Ordinal);
        Assert.Contains("같은 멱등 키의 동시 시작 경쟁", readme, StringComparison.Ordinal);
        Assert.Contains("`InventoryReserved` 이후 명시 재개", readme, StringComparison.Ordinal);
    }

    [Fact]
    public void ShoppingMall_Domain_Does_Not_Depend_On_Transport_Configuration_Or_Storage_Format()
    {
        var sampleRoot = ResolveSampleRoot("ShoppingMall");
        var domainFiles = Directory.GetFiles(
                Path.Combine(sampleRoot, "Server"),
                "*.cs",
                SearchOption.AllDirectories)
            .Where(static path => path.Contains(
                $"{Path.DirectorySeparatorChar}Domain{Path.DirectorySeparatorChar}",
                StringComparison.Ordinal));

        foreach (var domainFile in domainFiles)
        {
            var source = File.ReadAllText(domainFile);
            Assert.DoesNotContain("using ShoppingMall.Shared", source, StringComparison.Ordinal);
            Assert.DoesNotContain("using ShoppingMall.Server.Configuration", source, StringComparison.Ordinal);
            Assert.DoesNotContain("using System.Text.Json", source, StringComparison.Ordinal);
            Assert.DoesNotContain("JsonDerivedType", source, StringComparison.Ordinal);
        }

        var mapper = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Shared",
            "Contracts",
            "OrderContractMapper.cs"));
        Assert.Contains("OrderProjectionState", mapper, StringComparison.Ordinal);
        Assert.Contains("OrderState ToContract", mapper, StringComparison.Ordinal);
    }
}
