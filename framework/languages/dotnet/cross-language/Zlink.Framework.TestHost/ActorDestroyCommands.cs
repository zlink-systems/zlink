using System.Globalization;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.Contracts.Errors;

internal sealed class ActorDestroyCommands(
    TestHostOptions options,
    ActorDestroyEvents events,
    ActorDestroyStore store,
    IZLinkActorManager actors,
    IZLinkActorClient client,
    IZLinkFrameworkRuntime runtime,
    IZLinkRouteMeshRuntime mesh,
    IZLinkRouteMeshRuntimeOptions meshOptions,
    IHostApplicationLifetime lifetime
) : BackgroundService
{
    private sealed record Call(Task Api, Task Recorded);

    private readonly Dictionary<string, Call> _destroys = new(StringComparer.Ordinal);
    private ActorRef? _oldRef;
    private Call? _create;
    private Task? _destroyDeadline;
    private bool _finish;

    public override async Task StartAsync(CancellationToken cancellationToken)
    {
        await foreach (var observed in runtime.ObserveAsync(cancellationToken))
        {
            if (observed.Status.State == ZLinkFrameworkRuntimeState.Serving)
            {
                await base.StartAsync(cancellationToken);
                return;
            }
            if (
                observed.Status.State
                is ZLinkFrameworkRuntimeState.Error
                    or ZLinkFrameworkRuntimeState.Stopped
            )
                break;
        }
        throw new InvalidOperationException(
            "Framework stopped before the Actor destroy Entry Spot was ready."
        );
    }

    public override Task StopAsync(CancellationToken cancellationToken)
    {
        store.StopHolding();
        return base.StopAsync(cancellationToken);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!File.Exists(options.StartFilePath))
            await Task.Delay(10, stoppingToken);

        long previous = 0;
        var file = options.ActorDestroyControlPrefix + ".command";
        while (!_finish && !stoppingToken.IsCancellationRequested)
        {
            try
            {
                var line = await ReadCommandAsync(file, stoppingToken);
                var fields = line?.TrimEnd('\r', '\n').Split('\t');
                if (
                    fields is null
                    || fields[0] != options.ActorDestroyRunId
                    || (
                        fields.Length > 1
                        && long.TryParse(
                            fields[1],
                            NumberStyles.None,
                            CultureInfo.InvariantCulture,
                            out var seen
                        )
                        && seen <= previous
                    )
                )
                {
                    await Task.Delay(10, stoppingToken);
                    continue;
                }

                if (
                    fields.Length < 3
                    || fields[1] != (previous + 1).ToString(CultureInfo.InvariantCulture)
                )
                    throw new InvalidDataException($"Expected command sequence {previous + 1}.");
                await RunAsync(fields[2..], stoppingToken);
            }
            catch (Exception error) when (!stoppingToken.IsCancellationRequested)
            {
                events.Write("error", ("seq", previous + 1), ("message", error.ToString()));
                throw;
            }

            events.Write("ack", ("seq", ++previous));
        }

        if (_finish)
            lifetime.StopApplication();
    }

    private static async Task<string?> ReadCommandAsync(
        string file,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await using var stream = new FileStream(
                file,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete
            );
            using var reader = new StreamReader(stream);
            return await reader.ReadToEndAsync(cancellationToken);
        }
        catch (FileNotFoundException)
        {
            return null;
        }
    }

    private async Task RunAsync(string[] command, CancellationToken cancellationToken)
    {
        var verb = command[0];
        var arguments = verb
            is "placement"
                or "find"
                or "authority"
                or "destroy"
                or "pending-destroy"
                or "probe"
                or "lease"
            ? 1
            : 0;
        if (command.Length != arguments + 1 || arguments == 1 && string.IsNullOrEmpty(command[1]))
            throw new InvalidDataException(
                $"Invalid argument count or empty argument for '{verb}'."
            );

        switch (verb)
        {
            case "create":
                await CreateAsync(cancellationToken);
                break;
            case "placement":
                var weight = int.Parse(command[1], CultureInfo.InvariantCulture);
                meshOptions.Mesh(options.MeshName!).PlacementWeight = weight;
                await store.WaitForPlacementAsync(
                    RoutingId.From(options.NodeRid!),
                    weight,
                    cancellationToken
                );
                break;
            case "peer":
                await WaitForPeerAsync(cancellationToken);
                break;
            case "find":
                var actor = await actors.FindAsync(options.ActorId!, cancellationToken);
                if (actor is { } found)
                    await CaptureOriginalAsync(found, cancellationToken);
                events.Write(
                    "found",
                    ("label", command[1]),
                    ("generation", Format(actor?.ObjectGeneration))
                );
                break;
            case "authority":
                await store.AuthorityAsync(command[1], cancellationToken);
                break;
            case "hold-delete":
                store.HoldDelete();
                break;
            case "resume-delete":
                store.ResumeDelete();
                break;
            case "hold-cas":
                store.HoldCas();
                break;
            case "fail-cas":
                store.FailCas();
                break;
            case "destroy":
                var label = command[1];
                if (
                    label
                    is not ("remote" or "duplicate" or "stale" or "fail" or "retry" or "admission")
                )
                    throw new InvalidDataException($"Unknown destroy label '{label}'.");
                if (_destroys.ContainsKey(label))
                    throw new InvalidOperationException($"Destroy '{label}' already started.");
                var oldRef =
                    _oldRef
                    ?? throw new InvalidOperationException(
                        "The original ActorRef was not captured."
                    );
                if (label == "admission")
                    _destroyDeadline = RecordDestroyDeadlineAsync(cancellationToken);
                var destroy = actors.DestroyAsync(oldRef, cancellationToken).AsTask();
                _destroys.Add(label, new(destroy, RecordDestroyAsync(destroy, label)));
                break;
            case "pending-destroy":
                RequirePending(
                    _destroys.GetValueOrDefault(command[1])?.Api,
                    $"Destroy '{command[1]}'"
                );
                if (command[1] == "duplicate")
                    events.Write("ownerDuplicatePending");
                break;
            case "get-or-create":
                if (_create is not null)
                    throw new InvalidOperationException("GetOrCreate already started.");
                var create = StartGetOrCreate(cancellationToken);
                _create = new(create, RecordGetOrCreateAsync(create));
                break;
            case "pending-create":
                RequirePending(_create?.Api, "GetOrCreate");
                events.Write("createPending");
                break;
            case "probe":
                await ProbeAsync(command[1], cancellationToken);
                break;
            case "lease":
                events.Write(
                    "lease",
                    ("label", command[1]),
                    ("kind", await store.ReadLeaseAsync(cancellationToken))
                );
                break;
            case "wait-lease-expired":
                await WaitForLeaseExpiryAsync(cancellationToken);
                break;
            case "end-lease":
                await store.EndLeaseAsync(cancellationToken);
                break;
            case "allow-admission":
                await File.WriteAllTextAsync(
                    options.ActorDestroyControlPrefix + ".admission",
                    options.ActorDestroyRunId,
                    cancellationToken
                );
                break;
            case "finish":
                store.StopHolding();
                await Task.WhenAll(
                    _destroys
                        .Values.Select(call => call.Recorded)
                        .Append(_create?.Recorded ?? Task.CompletedTask)
                );
                if (_destroyDeadline is not null)
                    await _destroyDeadline;
                _finish = true;
                break;
            default:
                throw new InvalidDataException($"Unknown Actor destroy command '{verb}'.");
        }
    }

    private async Task WaitForPeerAsync(CancellationToken cancellationToken)
    {
        var peerRid = RoutingId.From(options.PeerRid!);
        await foreach (var observed in mesh.ObserveAsync(options.MeshName!, cancellationToken))
        {
            if (
                observed.Status.Peers.Any(peer =>
                    peer.NodeRid == peerRid && peer.State == ZLinkPeerState.Ready
                )
            )
                return;
        }
        throw new InvalidOperationException("Mesh status observation ended before peer admission.");
    }

    private async Task CreateAsync(CancellationToken cancellationToken)
    {
        var result = await actors
            .Create(options.ActorId!, ActorDestroyHost.ActorType)
            .InMesh(options.MeshName!)
            .Async(cancellationToken);
        if (
            result is not ZLinkActorCreateResult.Created created
            || created.Actor.NodeRid != RoutingId.From(options.NodeRid!)
        )
            throw new InvalidOperationException(
                $"Initial Create did not create an Actor on the local Entry Spot ({result})."
            );
        await CaptureOriginalAsync(created.Actor, cancellationToken);
        var probe = await RequestProbeAsync("create", cancellationToken);
        if (probe != Format(created.Actor.ObjectGeneration))
            throw new InvalidOperationException("Initial probe returned a different generation.");
        events.Write("created", ("generation", created.Actor.ObjectGeneration));
    }

    private async Task CaptureOriginalAsync(ActorRef actor, CancellationToken cancellationToken)
    {
        if (_oldRef is not null)
            return;
        await store.CaptureOriginalAsync(actor, cancellationToken);
        _oldRef = actor;
    }

    private async Task RecordDestroyAsync(Task<bool> destroy, string label)
    {
        var resultEvent = label switch
        {
            "duplicate" => "ownerDuplicateReturned",
            "fail" => "failedDestroyReturned",
            _ => "destroyReturned",
        };
        try
        {
            var result = await destroy;
            events.Write(resultEvent, ("label", label), ("kind", result ? "true" : "false"));
        }
        catch (Exception error)
        {
            if (
                label == "admission"
                && error
                    is ZLinkFrameworkException
                    {
                        Kind: ZLinkFrameworkErrorKind.Unavailable,
                        InnerException: ZlinkSubmitException
                        {
                            Result: ZlinkSubmitException.ErrorCode.NotConnected
                        }
                    }
            )
                events.Write("submissionRejected");
            var kind = error switch
            {
                ZLinkFrameworkException { InnerException: ActorDestroyClosingCasFailure } =>
                    "error",
                ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.DeadlineExceeded } =>
                    "deadline",
                ZLinkFrameworkException
                {
                    Kind: ZLinkFrameworkErrorKind.InvalidOperation
                        or ZLinkFrameworkErrorKind.Unavailable
                        or ZLinkFrameworkErrorKind.NotFound
                } failure => failure.Kind.ToString(),
                _ => "error",
            };
            events.Write(
                resultEvent,
                ("label", label),
                ("kind", kind),
                ("message", error.ToString())
            );
        }
    }

    private Task<ZLinkActorCreateResult> StartGetOrCreate(CancellationToken cancellationToken)
    {
        store.BeginCreate();
        try
        {
            return actors
                .GetOrCreate(options.ActorId!, ActorDestroyHost.ActorType)
                .InMesh(options.MeshName!)
                .Async(cancellationToken)
                .AsTask();
        }
        finally
        {
            store.EndCreate();
        }
    }

    private async Task RecordGetOrCreateAsync(Task<ZLinkActorCreateResult> create)
    {
        try
        {
            var result = await create;
            var (kind, actor) = result switch
            {
                ZLinkActorCreateResult.Created created => ("Created", created.Actor),
                ZLinkActorCreateResult.Existing existing => ("Existing", existing.Actor),
                _ => throw new InvalidOperationException(
                    $"Unexpected GetOrCreate result {result}."
                ),
            };
            events.Write(
                "getOrCreateReturned",
                ("kind", kind),
                ("generation", actor.ObjectGeneration)
            );
        }
        catch (Exception error)
        {
            events.Write(
                "getOrCreateReturned",
                ("kind", "error"),
                ("generation", "none"),
                ("message", error.ToString())
            );
        }
    }

    private async Task ProbeAsync(string label, CancellationToken cancellationToken)
    {
        var failedDestroy = _destroys.GetValueOrDefault("fail")?.Api;
        var retryStarted = _destroys.ContainsKey("retry");
        if (events.Scenario == "e" && failedDestroy?.IsCompleted == true && !retryStarted)
            events.RetryProbeLabel = label;
        (string Name, object? Value)[] fields;
        try
        {
            var reply = await RequestProbeAsync(label, cancellationToken);
            fields = [("label", label), ("kind", "return"), ("generation", reply)];
        }
        catch (Exception error)
        {
            var kind = error
                is ZLinkFrameworkException
                {
                    Kind: ZLinkFrameworkErrorKind.Unavailable or ZLinkFrameworkErrorKind.NotFound
                } failure
                ? failure.Kind.ToString()
                : "error";
            fields =
            [
                ("label", label),
                ("kind", kind),
                ("retryable", kind == "Unavailable" ? "true" : "false"),
                ("message", error.ToString()),
            ];
        }
        events.Write("probeReturned", fields);
        if (events.Scenario == "e")
        {
            if (failedDestroy?.IsCompleted == false)
                events.Write("pendingCasProbeReturned", fields);
            if (retryStarted)
                events.Write("postCommitProbeReturned", fields);
        }
    }

    private ValueTask<string> RequestProbeAsync(
        string label,
        CancellationToken cancellationToken
    ) =>
        client
            .RequestToActor(options.ActorId!, new ActorDestroyProbeReq(label))
            .Async<string>(cancellationToken);

    private async Task WaitForLeaseExpiryAsync(CancellationToken cancellationToken)
    {
        while (true)
        {
            var kind = await store.ReadLeaseAsync(cancellationToken);
            if (kind == "expired")
            {
                events.Write("ownerLeaseExpired");
                return;
            }
            if (kind != "valid")
                throw new InvalidOperationException(
                    $"Original lease is {kind}, expected natural expiration."
                );
            await Task.Delay(10, cancellationToken);
        }
    }

    private async Task RecordDestroyDeadlineAsync(CancellationToken cancellationToken)
    {
        // DestroyAsync uses the framework default request timeout.
        await Task.Delay(events.RequestTimeout, cancellationToken);
        events.Write("destroyDeadline");
    }

    private static void RequirePending(Task? operation, string name)
    {
        if (operation is null || operation.IsCompleted)
            throw new InvalidOperationException($"{name} is not pending.");
    }

    private static string Format(ulong? generation) =>
        generation?.ToString(CultureInfo.InvariantCulture) ?? "none";
}
