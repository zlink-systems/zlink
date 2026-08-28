using System.Diagnostics.Metrics;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class RuntimeMetricsTests
{
    [Fact]
    public void Meter_Catalog_Uses_Exact_Names_Kinds_Units_And_Scope()
    {
        var instruments = new List<Instrument>();
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, _) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework)
                    instruments.Add(instrument);
            }
        };

        listener.Start();
        ZLinkRuntimeMetrics.RecordActorCreated("mesh");

        var expected = new Dictionary<string, (Type GenericType, string? Unit)>(StringComparer.Ordinal)
        {
            ["zlink.stream.connections.active"] = (typeof(UpDownCounter<>), "{connection}"),
            ["zlink.stream.connections.opened"] = (typeof(Counter<>), "{connection}"),
            ["zlink.stream.connections.closed"] = (typeof(Counter<>), "{connection}"),
            ["zlink.mesh_node.peers.configured"] = (typeof(ObservableGauge<>), "{peer}"),
            ["zlink.mesh_node.peers.connected"] = (typeof(ObservableGauge<>), "{peer}"),
            ["zlink.mesh_node.peers.ready"] = (typeof(ObservableGauge<>), "{peer}"),
            ["zlink.mesh_node.channels.ready_members"] = (typeof(ObservableGauge<>), "{member}"),
            ["zlink.mesh_node.channel.selection_failures"] = (typeof(Counter<>), "{failure}"),
            ["zlink.mesh_node.requests.inflight"] = (typeof(UpDownCounter<>), "{request}"),
            ["zlink.mesh_node.request.duration"] = (typeof(Histogram<>), "s"),
            ["zlink.mesh_node.request.timeouts"] = (typeof(Counter<>), "{request}"),
            ["zlink.mesh_node.messages.dropped"] = (typeof(Counter<>), "{message}"),
            ["zlink.object.capacity.active"] = (typeof(ObservableGauge<>), "{object}"),
            ["zlink.object.capacity.reserved"] = (typeof(ObservableGauge<>), "{object}"),
            ["zlink.object.capacity.limit"] = (typeof(ObservableGauge<>), "{object}"),
            ["zlink.spot.type.capacity.active"] = (typeof(ObservableGauge<>), "{spot}"),
            ["zlink.spot.type.capacity.reserved"] = (typeof(ObservableGauge<>), "{spot}"),
            ["zlink.spot.type.capacity.limit"] = (typeof(ObservableGauge<>), "{spot}"),
            ["zlink.object.activation.active"] = (typeof(ObservableGauge<>), "{activation}"),
            ["zlink.object.activation.limit"] = (typeof(ObservableGauge<>), "{activation}"),
            ["zlink.spot.count"] = (typeof(UpDownCounter<>), "{spot}"),
            ["zlink.actor.count"] = (typeof(UpDownCounter<>), "{actor}"),
            ["zlink.relocation.started"] = (typeof(Counter<>), "{relocation}"),
            ["zlink.relocation.completed"] = (typeof(Counter<>), "{relocation}"),
            ["zlink.relocation.duration"] = (typeof(Histogram<>), "s"),
            ["zlink.relocation.bytes"] = (typeof(Histogram<>), "By"),
            ["zlink.relocation.interruption"] =
                (typeof(Histogram<>), "s"),
            ["zlink.relocation.target_resume"] = (typeof(Histogram<>), "s"),
            ["zlink.relocation.route_convergence"] = (typeof(Histogram<>), "s"),
            ["zlink.relocation.cutover_timeout"] =
                (typeof(Counter<>), "{fallback}"),
            ["zlink.instance_spot.activations"] = (typeof(Counter<>), "{activation}"),
            ["zlink.instance_spot.activation.duration"] = (typeof(Histogram<>), "s"),
            ["zlink.instance_spot.pending.messages"] = (typeof(ObservableGauge<>), "{message}"),
            ["zlink.instance_spot.pending.bytes"] = (typeof(ObservableGauge<>), "By"),
            ["zlink.instance_spot.claim.conflicts"] = (typeof(Counter<>), "{claim}"),
            ["zlink.instance_spot.takeovers"] = (typeof(Counter<>), "{takeover}"),
            ["zlink.location.store.errors"] = (typeof(Counter<>), "{error}"),
            ["zlink.location.owner_lease.renew.failures"] = (typeof(Counter<>), "{failure}"),
            ["zlink.location.owner_lease.renew.lateness"] = (typeof(Histogram<>), "s"),
            ["zlink.observability.events.overflow"] = (typeof(Counter<>), "{event}"),
            ["zlink.host.state"] = (typeof(ObservableGauge<>), "{runtime}"),
            ["zlink.host.core_hwm.effective_budget"] = (typeof(ObservableGauge<>), "By"),
            ["zlink.host.core_hwm.applied"] = (typeof(ObservableGauge<>), "By"),
            ["zlink.host.core_hwm.accounted"] = (typeof(ObservableGauge<>), "By"),
            ["zlink.host.core_hwm.completion_accounted"] = (typeof(ObservableGauge<>), "By"),
            ["zlink.host.core_hwm.blocked_ratio"] = (typeof(ObservableGauge<>), "{ppm}"),
            ["zlink.host.application_job_queue.limit"] = (typeof(ObservableGauge<>), "{job}"),
            ["zlink.host.application_job_queue.jobs"] = (typeof(ObservableGauge<>), "{job}"),
            ["zlink.host.application_job_queue.capacity_waiters"] = (typeof(ObservableGauge<>), "{waiter}"),
            ["zlink.host.application_job_queue.capacity_waits"] = (typeof(ObservableCounter<>), "{wait}"),
            ["zlink.host.application_job_queue.capacity_wait_duration"] = (typeof(ObservableCounter<>), "s"),
            ["zlink.host.application_job_queue.pressure_state"] = (typeof(ObservableGauge<>), "{state}"),
            ["zlink.host.application_job_queue.pressure_transitions"] = (typeof(ObservableCounter<>), "{transition}"),
            ["zlink.host.application_job_queue.pause_duration"] = (typeof(ObservableGauge<>), "s"),
            ["zlink.host.application_job_queue.flow_state_config_failures"] = (typeof(ObservableCounter<>), "{failure}"),
            ["zlink.host.relocation.duration"] = (typeof(Histogram<>), "s"),
            ["zlink.host.relocation.blocked"] = (typeof(Counter<>), "{operation}"),
            ["zlink.host.shutdown.duration"] = (typeof(Histogram<>), "s"),
            ["zlink.host.shutdown.forced"] = (typeof(Counter<>), "{operation}")
        };

        Assert.Equal(expected.Count, instruments.Count);

        foreach (var (name, contract) in expected)
        {
            Assert.Contains(
                instruments,
                instrument => instrument.Name == name
                              && instrument.GetType().GetGenericTypeDefinition() == contract.GenericType
                              && instrument.Unit == contract.Unit);
        }

        Assert.DoesNotContain(
            instruments,
            instrument => instrument.Name.StartsWith(
                "zlink.fanout.",
                StringComparison.Ordinal));
        Assert.DoesNotContain(
            instruments,
            instrument => instrument.Name is
                "zlink.spot.queue.depth"
                or "zlink.spot.queue.wait.duration"
                or "zlink.actor.mailbox.depth");
        Assert.DoesNotContain(
            instruments,
            instrument => instrument.Name.StartsWith(
                "zlink.actor.transfer",
                StringComparison.Ordinal));
        Assert.DoesNotContain(
            instruments,
            instrument => instrument.Name == "zlink.actor.transfers");
    }

    [Fact]
    public void Listener_Failure_Does_Not_Change_Runtime_Result()
    {
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name == "zlink.actor.count")
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<long>(static (_, _, _, _) =>
            throw new InvalidOperationException("application listener failure"));
        listener.Start();

        var exception = Record.Exception(
            () => ZLinkRuntimeMetrics.RecordActorCreated("mesh"));

        Assert.Null(exception);
    }

    [Fact]
    public void Host_capacity_metrics_read_one_projection_with_only_exact_state_labels()
    {
        var core = default(ZLinkCoreHwmStatus) with
        {
            EffectiveBudgetBytes = 100,
            TotalAppliedHwmBytes = 80,
            CurrentAccountedBytes = 30,
            PeakAccountedBytes = 50,
            CompletionCurrentAccountedBytes = 3,
            CompletionPeakAccountedBytes = 5,
            BlockedRatioPpm = 7
        };
        var queue = new ZLinkApplicationJobQueueStatus(
            ZLinkApplicationJobQueueProfile.Balanced,
            ConfiguredManualMax: 16,
            ConfiguredPauseThresholdPercent: 80,
            ConfiguredResumeThresholdPercent: 60,
            EffectiveProcessorCount: 4,
            EffectiveMaxQueuedApplicationJobs: 16,
            PausePermitCount: 13,
            ResumePermitCount: 9,
            ReservedSupplyPermits: 2,
            QueuedApplicationJobs: 3,
            PermitsInUse: 5,
            PeakPermitsInUse: 8,
            CapacityWaiters: 1,
            CapacityWaitCount: 11,
            CapacityWaitDuration: TimeSpan.FromSeconds(1.25),
            PressureState: ZLinkApplicationJobQueuePressureState.Running,
            CurrentPauseDuration: TimeSpan.Zero);
        using var registration = ZLinkRuntimeMetrics.RegisterHostCapacity(
            () => new ZLinkHostCapacityStatus(4, core, queue));
        var longSamples = new List<(
            string Name,
            long Value,
            IReadOnlyDictionary<string, string> Tags)>();
        var durationSamples = new List<double>();
        var names = new[]
        {
            "zlink.host.core_hwm.effective_budget",
            "zlink.host.core_hwm.applied",
            "zlink.host.core_hwm.accounted",
            "zlink.host.core_hwm.completion_accounted",
            "zlink.host.core_hwm.blocked_ratio",
            "zlink.host.application_job_queue.limit",
            "zlink.host.application_job_queue.jobs",
            "zlink.host.application_job_queue.capacity_waiters",
            "zlink.host.application_job_queue.capacity_waits"
        };
        using var longListener = Listen<long>(
            names,
            (instrument, value, tags) => longSamples.Add(
                (instrument.Name, value, Tags(tags))));
        using var durationListener = Listen<double>(
            "zlink.host.application_job_queue.capacity_wait_duration",
            (_, value, _) => durationSamples.Add(value));

        longListener.RecordObservableInstruments();
        durationListener.RecordObservableInstruments();

        Assert.Contains(longSamples, sample =>
            sample.Name == "zlink.host.core_hwm.effective_budget"
            && sample.Value == 100
            && sample.Tags.Count == 0);
        Assert.Equal(
            ["current", "peak"],
            longSamples
                .Where(static sample =>
                    sample.Name == "zlink.host.core_hwm.accounted")
                .Select(static sample => sample.Tags["state"])
                .Order(StringComparer.Ordinal));
        Assert.Equal(
            ["in_use", "peak", "queued", "reserved"],
            longSamples
                .Where(static sample =>
                    sample.Name == "zlink.host.application_job_queue.jobs")
                .Select(static sample => sample.Tags["state"])
                .Order(StringComparer.Ordinal));
        Assert.All(
            longSamples,
            static sample => Assert.All(
                sample.Tags.Keys,
                static key => Assert.Equal("state", key)));
        Assert.Equal(1.25, Assert.Single(durationSamples), precision: 3);
    }

    [Fact]
    public void Application_job_pressure_metrics_project_exact_states_and_durations()
    {
        using var registration =
            ZLinkRuntimeMetrics.RegisterApplicationJobQueuePressure(
                () => new ZLinkApplicationJobQueuePressureMetrics(
                    ZLinkApplicationJobQueuePressureState.Paused,
                    PausedTransitionCount: 4,
                    RunningTransitionCount: 3,
                    CurrentPauseDuration: TimeSpan.FromSeconds(5),
                    CumulativePauseDuration: TimeSpan.FromSeconds(9),
                    FlowStateConfigFailures: 2));
        var longSamples = new List<(
            string Name,
            long Value,
            IReadOnlyDictionary<string, string> Tags)>();
        var durationSamples = new List<(
            double Value,
            IReadOnlyDictionary<string, string> Tags)>();
        using var longListener = Listen<long>(
            [
                "zlink.host.application_job_queue.pressure_state",
                "zlink.host.application_job_queue.pressure_transitions",
                "zlink.host.application_job_queue.flow_state_config_failures"
            ],
            (instrument, value, tags) => longSamples.Add(
                (instrument.Name, value, Tags(tags))));
        using var durationListener = Listen<double>(
            "zlink.host.application_job_queue.pause_duration",
            (_, value, tags) => durationSamples.Add((value, Tags(tags))));

        longListener.RecordObservableInstruments();
        durationListener.RecordObservableInstruments();

        Assert.Contains(longSamples, sample =>
            sample.Name == "zlink.host.application_job_queue.pressure_state"
            && sample.Value == 1
            && sample.Tags["state"] == "paused");
        Assert.DoesNotContain(longSamples, sample =>
            sample.Name == "zlink.host.application_job_queue.pressure_state"
            && sample.Tags["state"] == "running");
        Assert.Contains(longSamples, sample =>
            sample.Name == "zlink.host.application_job_queue.pressure_transitions"
            && sample.Value == 4
            && sample.Tags["state"] == "paused");
        Assert.Contains(longSamples, sample =>
            sample.Name == "zlink.host.application_job_queue.pressure_transitions"
            && sample.Value == 3
            && sample.Tags["state"] == "running");
        Assert.Contains(longSamples, sample =>
            sample.Name == "zlink.host.application_job_queue.flow_state_config_failures"
            && sample.Value == 2
            && sample.Tags.Count == 0);
        Assert.Contains(durationSamples, sample =>
            sample.Value == 5 && sample.Tags["state"] == "current");
        Assert.Contains(durationSamples, sample =>
            sample.Value == 9 && sample.Tags["state"] == "cumulative");
    }

    [Fact]
    public void Disabled_Host_Operation_Remains_Safe()
    {
        var operation = ZLinkRuntimeMetrics.StartHostShutdown();
        operation.Complete("stopped", "none");
        operation.Complete("force_stopped", "deadline_exceeded");
    }

    [Fact]
    public void Relocation_Interruption_Start_Matches_Observation_State()
    {
        var observer = new ZLinkRelocationInterruptionObserver(
            loggerFactory: null);

        var operation = observer.Start(
            ZLinkRelocationUnitKind.Actor,
            "entry");

        Assert.Equal(
            ZLinkRuntimeMetrics.RelocationInterruptionEnabled,
            operation.Enabled);
        operation.Complete();
    }

    [Fact]
    public void Relocation_Interruption_Records_Unit_And_Execution_Mode()
    {
        var measurements =
            new List<(double Value, KeyValuePair<string, object?>[] Tags)>();
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name
                    == "zlink.relocation.interruption")
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<double>(
            (_, value, tags, _) =>
                measurements.Add((value, tags.ToArray())));
        listener.Start();
        var time = new ManualTimeProvider();
        var observer = new ZLinkRelocationInterruptionObserver(
            loggerFactory: null,
            time);

        var operation = observer.Start(
            ZLinkRelocationUnitKind.Actor,
            "entry");
        time.Advance(TimeSpan.FromMilliseconds(750));
        operation.Complete();
        operation.Complete();

        var measurement = Assert.Single(measurements);
        Assert.Equal(0.75, measurement.Value, precision: 3);
        Assert.Contains(
            measurement.Tags,
            tag => tag.Key == "unit_kind"
                   && Equals(tag.Value, "actor"));
        Assert.Contains(
            measurement.Tags,
            tag => tag.Key == "execution_mode"
                   && Equals(tag.Value, "entry"));
    }

    [Fact]
    public void Interruption_Target_Warning_Does_Not_Include_Object_Identifiers()
    {
        var logs = new RecordingLoggerProvider();
        using var loggerFactory = LoggerFactory.Create(builder =>
            builder.SetMinimumLevel(LogLevel.Warning).AddProvider(logs));
        var time = new ManualTimeProvider();
        var observer = new ZLinkRelocationInterruptionObserver(
            loggerFactory,
            time);

        var operation = observer.Start(
            ZLinkRelocationUnitKind.UserSpot,
            "per_actor");
        time.Advance(TimeSpan.FromMilliseconds(1_250));
        operation.Complete();

        var log = Assert.Single(logs.Entries);
        Assert.Equal(LogLevel.Warning, log.Level);
        Assert.Equal("zlink.runtime.relocation.changed", log.EventId.Name);
        Assert.Equal(
            "user_spot",
            log.State["UnitKind"]);
        Assert.Equal(
            "per_actor",
            log.State["ExecutionMode"]);
        Assert.Equal(true, log.State["InterruptionTargetExceeded"]);
        Assert.Equal(
            1.25,
            Assert.IsType<double>(log.State["DurationSeconds"]),
            precision: 3);
        Assert.DoesNotContain(
            log.State.Keys,
            key => key.Contains("ActorId", StringComparison.Ordinal)
                   || key.Contains("SpotId", StringComparison.Ordinal));
    }

    [Fact]
    public void Interruption_Warning_Provider_Failure_Does_Not_Escape()
    {
        using var loggerFactory = LoggerFactory.Create(builder =>
            builder.SetMinimumLevel(LogLevel.Warning)
                .AddProvider(new ThrowingLoggerProvider(
                    throwFromIsEnabled: false)));
        var time = new ManualTimeProvider();
        var observer = new ZLinkRelocationInterruptionObserver(
            loggerFactory,
            time);
        var operation = observer.Start(
            ZLinkRelocationUnitKind.Actor,
            "entry");
        time.Advance(TimeSpan.FromMilliseconds(1_250));

        var exception = Record.Exception(operation.Complete);

        Assert.Null(exception);
    }

    [Fact]
    public void Interruption_Warning_IsEnabled_Failure_Does_Not_Escape()
    {
        using var loggerFactory = LoggerFactory.Create(builder =>
            builder.SetMinimumLevel(LogLevel.Warning)
                .AddProvider(new ThrowingLoggerProvider(
                    throwFromIsEnabled: true)));
        var observer = new ZLinkRelocationInterruptionObserver(
            loggerFactory);

        var exception = Record.Exception(() =>
        {
            var operation = observer.Start(
                ZLinkRelocationUnitKind.Actor,
                "entry");
            operation.Complete();
        });

        Assert.Null(exception);
    }

    [Fact]
    public void Interruption_Logger_Creation_Failure_Does_Not_Escape()
    {
        var exception = Record.Exception(() =>
        {
            var observer = new ZLinkRelocationInterruptionObserver(
                new ThrowingLoggerFactory());
            observer.Start(
                    ZLinkRelocationUnitKind.Actor,
                    "entry")
                .Complete();
        });

        Assert.Null(exception);
    }

    [Fact]
    public void Inactive_Meter_Does_Not_Allocate_Or_Retain_Per_Event_State()
    {
        ZLinkRuntimeMetrics.RecordLocationStoreError("read");
        var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();

        for (var index = 0; index < 1_000_000; index++)
            ZLinkRuntimeMetrics.RecordLocationStoreError("read");

        Assert.Equal(allocatedBefore, GC.GetAllocatedBytesForCurrentThread());
    }

    [Fact]
    public async Task Actor_Transfer_Pending_Count_Excludes_One_Way_Send()
    {
        var mailbox = new ZLinkActorSerialExecutor();
        var active = await mailbox.EnterAsync(CancellationToken.None);
        var pendingSend = mailbox.EnterAsync(
                CancellationToken.None)
            .AsTask();
        var pendingRequest = mailbox.EnterAsync(
                CancellationToken.None,
                countAsPendingRequest: true)
            .AsTask();

        Assert.Equal(1, mailbox.PendingRequestCount);

        active.Dispose();
        (await pendingSend).Dispose();
        (await pendingRequest).Dispose();
        Assert.Equal(0, mailbox.PendingRequestCount);
    }

    [Fact]
    public void Relocation_Metric_Uses_Closed_Labels_And_Records_Terminal_Once()
    {
        var counterSamples = new List<(string Name, IReadOnlyDictionary<string, string> Tags)>();
        var durationSamples =
            new List<(double Value, IReadOnlyDictionary<string, string> Tags)>();
        using var counterListener = Listen<long>(
            ["zlink.relocation.started", "zlink.relocation.completed"],
            (instrument, _, tags) => counterSamples.Add((instrument.Name, Tags(tags))));
        using var durationListener = Listen<double>(
            "zlink.relocation.duration",
            (_, value, tags) => durationSamples.Add((value, Tags(tags))));

        var operation = ZLinkRuntimeMetrics.StartRelocation(
            "game",
            ZLinkRelocationMetricObjectKind.Actor,
            ZLinkRelocationMetricPolicy.Snapshot);
        operation.Complete(ZLinkRelocationMetricOutcome.Aborted);
        operation.Complete(ZLinkRelocationMetricOutcome.Completed);

        var started = Assert.Single(
            counterSamples,
            sample => sample.Name == "zlink.relocation.started");
        Assert.Equal(
            new Dictionary<string, string>
            {
                ["mesh_name"] = "game",
                ["object_kind"] = "actor",
                ["policy"] = "snapshot"
            },
            started.Tags);
        var completed = Assert.Single(
            counterSamples,
            sample => sample.Name == "zlink.relocation.completed");
        Assert.Equal("game", completed.Tags["mesh_name"]);
        Assert.Equal("actor", completed.Tags["object_kind"]);
        Assert.Equal("snapshot", completed.Tags["policy"]);
        Assert.Equal("aborted", completed.Tags["outcome"]);
        var duration = Assert.Single(durationSamples);
        Assert.True(duration.Value >= 0);
        Assert.Equal(completed.Tags, duration.Tags);
    }

    [Fact]
    public void Relocation_Metric_Uses_Only_The_Closed_Terminal_Outcomes()
    {
        var terminal = new List<IReadOnlyDictionary<string, string>>();
        using var listener = Listen<long>(
            ["zlink.relocation.completed"],
            (_, _, tags) => terminal.Add(Tags(tags)));

        var outcomes = new[]
        {
            ZLinkRelocationMetricOutcome.Completed,
            ZLinkRelocationMetricOutcome.Aborted,
            ZLinkRelocationMetricOutcome.Recovered,
            ZLinkRelocationMetricOutcome.Failed,
            ZLinkRelocationMetricOutcome.Shutdown
        };
        foreach (var outcome in outcomes)
            ZLinkRuntimeMetrics.StartRelocation(
                    "mesh",
                    ZLinkRelocationMetricObjectKind.UserSpot,
                    ZLinkRelocationMetricPolicy.Recreate)
                .Complete(outcome);

        //  `recovered` stays an allowed outcome value even though the derived
        //  counter was removed from the spec.
        Assert.Equal(
            ["completed", "aborted", "recovered", "failed", "shutdown"],
            terminal.Select(static sample => sample["outcome"]));
    }

    [Fact]
    public void Relocation_Metric_Records_One_Terminal_When_Outcomes_Compete()
    {
        var terminalCount = 0;
        var durationCount = 0;
        using var terminalListener = Listen<long>(
            "zlink.relocation.completed",
            (_, _, _) => Interlocked.Increment(ref terminalCount));
        using var durationListener = Listen<double>(
            "zlink.relocation.duration",
            (_, _, _) => Interlocked.Increment(ref durationCount));
        var operation = ZLinkRuntimeMetrics.StartRelocation(
            "mesh",
            ZLinkRelocationMetricObjectKind.Actor,
            ZLinkRelocationMetricPolicy.Recreate);
        var outcomes = Enum.GetValues<ZLinkRelocationMetricOutcome>();

        Parallel.For(
            0,
            256,
            index => operation.Complete(outcomes[index % outcomes.Length]));

        Assert.Equal(1, Volatile.Read(ref terminalCount));
        Assert.Equal(1, Volatile.Read(ref durationCount));
    }

    [Fact]
    public void Relocation_Bytes_Uses_Its_Exact_Label_Set()
    {
        var samples = new List<(string Name, long Value, IReadOnlyDictionary<string, string> Tags)>();
        using var listener = Listen<long>(
            ["zlink.relocation.bytes"],
            (instrument, value, tags) => samples.Add((instrument.Name, value, Tags(tags))));

        var operation = ZLinkRuntimeMetrics.StartRelocation(
            "mesh",
            ZLinkRelocationMetricObjectKind.InstanceSpot,
            ZLinkRelocationMetricPolicy.Snapshot);
        operation.RecordBytes(4096);
        operation.Complete(ZLinkRelocationMetricOutcome.Completed);

        var bytes = Assert.Single(
            samples,
            sample => sample.Name == "zlink.relocation.bytes");
        Assert.Equal(4096, bytes.Value);
        Assert.Equal("mesh", bytes.Tags["mesh_name"]);
        Assert.Equal("instance_spot", bytes.Tags["object_kind"]);
        Assert.Equal("snapshot", bytes.Tags["policy"]);
    }

    [Fact]
    public void Request_Metrics_Close_Inflight_And_Count_Only_Timeouts()
    {
        var inflight = new List<long>();
        var durations = new List<double>();
        var timeouts = new List<long>();
        using var inflightListener = Listen<long>(
            "zlink.mesh_node.requests.inflight",
            (_, value, _) => inflight.Add(value));
        using var durationListener = Listen<double>(
            "zlink.mesh_node.request.duration",
            (_, value, _) => durations.Add(value));
        using var timeoutListener = Listen<long>(
            "zlink.mesh_node.request.timeouts",
            (_, value, _) => timeouts.Add(value));

        var completed = ZLinkRuntimeMetrics.StartRequest("mesh", "channel");
        completed.Complete("completed");
        var timedOut = ZLinkRuntimeMetrics.StartRequest("mesh", "channel");
        timedOut.Complete("timed_out");

        Assert.Equal([1L, -1L, 1L, -1L], inflight);
        Assert.Equal(2, durations.Count);
        Assert.Equal([1L], timeouts);
    }

    [Fact]
    public void Spot_Count_Keeps_Mesh_And_Spot_Kind_Separate()
    {
        var samples = new List<(string Name, long Value, string? Kind)>();
        using var listener = Listen<long>(
            "zlink.spot.count",
            (instrument, value, tags) => samples.Add((instrument.Name, value, Tag(tags, "spot_kind"))));

        ZLinkRuntimeMetrics.RecordSpotCreated("mesh", "entry");
        ZLinkRuntimeMetrics.RecordSpotCreated("mesh", "user");
        ZLinkRuntimeMetrics.RecordSpotClosed("mesh", "entry");
        ZLinkRuntimeMetrics.RecordSpotClosed("mesh", "user");

        Assert.Equal(0, samples.Where(sample => sample.Name == "zlink.spot.count" && sample.Kind == "entry").Sum(sample => sample.Value));
        Assert.Equal(0, samples.Where(sample => sample.Name == "zlink.spot.count" && sample.Kind == "user").Sum(sample => sample.Value));
    }

    [Fact]
    public void Location_And_Dropped_Metrics_Use_Closed_Labels_And_Ignore_Listener_Failure()
    {
        var samples = new List<(string Name, IReadOnlyDictionary<string, string> Tags)>();
        using var listener = Listen<long>(
            [
                "zlink.location.owner_lease.renew.failures",
                "zlink.mesh_node.messages.dropped"
            ],
            (instrument, _, tags) => samples.Add((instrument.Name, Tags(tags))));

        ZLinkRuntimeMetrics.RecordOwnerLeaseRenewFailure("mesh", "mesh");
        ZLinkRuntimeMetrics.RecordMessageDropped(
            "mesh",
            "channel",
            "request",
            "stale_target");

        Assert.Contains(samples, sample => sample.Name == "zlink.location.owner_lease.renew.failures");
        var dropped = Assert.Single(samples, sample => sample.Name == "zlink.mesh_node.messages.dropped");
        Assert.Equal("channel", dropped.Tags["surface"]);
        Assert.Equal("request", dropped.Tags["message_kind"]);
        Assert.Equal("stale_target", dropped.Tags["reason"]);
    }

    [Fact]
    public void Stream_Close_Uses_Closed_Transport_And_Reason_Labels()
    {
        var samples = new List<IReadOnlyDictionary<string, string>>();
        using var listener = Listen<long>(
            "zlink.stream.connections.closed",
            (_, _, tags) => samples.Add(Tags(tags)));

        ZLinkRuntimeMetrics.RecordStreamClosed("tcp", "server_drain");

        var sample = Assert.Single(samples);
        Assert.Equal("tcp", sample["transport"]);
        Assert.Equal("server_shutdown", sample["close_reason"]);
    }

    [Fact]
    public void Forced_Shutdown_Is_Recorded_Once_With_Reason()
    {
        var samples = new List<(long Value, string? Reason)>();
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name == "zlink.host.shutdown.forced")
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<long>((instrument, value, tags, _) =>
        {
            if (instrument.Name == "zlink.host.shutdown.forced")
                samples.Add((value, Tag(tags, "reason")));
        });
        listener.Start();

        var operation = ZLinkRuntimeMetrics.StartHostShutdown();
        operation.Complete("force_stopped", "deadline_exceeded");
        operation.Complete("force_stopped", "teardown_failed");

        Assert.Equal([(1L, (string?)"deadline_exceeded")], samples);
    }

    [Fact]
    public void Observable_Metrics_Pull_Current_Source_State_After_Listener_Attaches()
    {
        long firstPeers = 2;
        var hostState = "serving";
        using var first = ZLinkRuntimeMetrics.RegisterMeshSnapshots(
            () =>
            [
                new ZLinkRuntimeMetricMeshSnapshot(
                    "mesh",
                    "manual",
                    firstPeers,
                    1,
                    1,
                    [],
                    new ZLinkRuntimeMetricCapacity(0, 0, 100),
                    new ZLinkRuntimeMetricCapacity(0, 0, 100),
                    [],
                    new ZLinkRuntimeMetricActivation(0, 10),
                    [])
            ]);
        using var host = ZLinkRuntimeMetrics.RegisterHostState(() => hostState);
        var peerSamples = new List<long>();
        var hostSamples = new List<string?>();
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name is "zlink.mesh_node.peers.configured" or "zlink.host.state")
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<long>((instrument, value, tags, _) =>
        {
            if (instrument.Name == "zlink.mesh_node.peers.configured") peerSamples.Add(value);
            if (instrument.Name == "zlink.host.state") hostSamples.Add(Tag(tags, "state"));
        });
        listener.Start();

        listener.RecordObservableInstruments();
        Assert.Contains(2, peerSamples);
        Assert.Contains("serving", hostSamples);

        firstPeers = 4;
        hostState = "draining";
        listener.RecordObservableInstruments();
        Assert.Equal(4, peerSamples[^1]);
        Assert.Equal("draining", hostSamples[^1]);
    }

    private static string? Tag(ReadOnlySpan<KeyValuePair<string, object?>> tags, string name)
    {
        foreach (var tag in tags)
            if (tag.Key == name) return tag.Value as string;
        return null;
    }

    private static IReadOnlyDictionary<string, string> Tags(
        ReadOnlySpan<KeyValuePair<string, object?>> tags)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var tag in tags) result[tag.Key] = tag.Value?.ToString() ?? string.Empty;
        return result;
    }

    private static MeterListener Listen<T>(
        string instrumentName,
        MetricRecorder<T> record)
        where T : struct => Listen([instrumentName], record);

    private static MeterListener Listen<T>(
        IReadOnlyCollection<string> instrumentNames,
        MetricRecorder<T> record)
        where T : struct
    {
        var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrumentNames.Contains(instrument.Name))
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<T>((instrument, value, tags, _) => record(instrument, value, tags));
        listener.Start();
        return listener;
    }

    private sealed class RecordingLoggerProvider : ILoggerProvider
    {
        internal List<LogEntry> Entries { get; } = [];

        public ILogger CreateLogger(string categoryName) =>
            new RecordingLogger(Entries);

        public void Dispose()
        {
        }
    }

    private sealed class ThrowingLoggerProvider(bool throwFromIsEnabled)
        : ILoggerProvider
    {
        public ILogger CreateLogger(string categoryName) =>
            new ThrowingLogger(throwFromIsEnabled);

        public void Dispose()
        {
        }
    }

    private sealed class ThrowingLoggerFactory : ILoggerFactory
    {
        public void AddProvider(ILoggerProvider provider)
        {
        }

        public ILogger CreateLogger(string categoryName) =>
            throw new InvalidOperationException(
                "injected CreateLogger failure");

        public void Dispose()
        {
        }
    }

    private sealed class ThrowingLogger(bool throwFromIsEnabled) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) =>
            throwFromIsEnabled
                ? throw new InvalidOperationException(
                    "injected IsEnabled failure")
                : true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter) =>
            throw new InvalidOperationException("injected logger failure");
    }

    private sealed class RecordingLogger(List<LogEntry> entries) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) =>
            logLevel >= LogLevel.Warning;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            var values = state is IEnumerable<KeyValuePair<string, object?>>
                structured
                ? structured.ToDictionary(
                    static pair => pair.Key,
                    static pair => pair.Value,
                    StringComparer.Ordinal)
                : [];
            entries.Add(new LogEntry(logLevel, eventId, values));
        }
    }

    private sealed record LogEntry(
        LogLevel Level,
        EventId EventId,
        IReadOnlyDictionary<string, object?> State);

    private delegate void MetricRecorder<T>(
        Instrument instrument,
        T value,
        ReadOnlySpan<KeyValuePair<string, object?>> tags)
        where T : struct;
}
