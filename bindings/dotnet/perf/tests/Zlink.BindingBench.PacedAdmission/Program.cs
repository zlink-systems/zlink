// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink;
using static PerfRunner;

CheckRotation();
CheckReplyTurn();
CheckCatchUp();
ZeroRateCheck.Run();
CheckSchedule();

const int Size = 64;
const int ExpectedHealthy = 4;
var testTimeout = TimeSpan.FromSeconds(1);
using var context = Zlink.CreateContext();
context.Options.AutoHwmEnabled = false;
using var poller = new PollManager();
using var blocked = context.CreateDealerSocket();
using var healthy = context.CreateDealerSocket();
using var blockedReceiver = context.CreateDealerSocket();
using var healthyReceiver = context.CreateDealerSocket();
Connect(blocked, blockedReceiver, "inproc://perf-paced-blocked", 128);
Connect(healthy, healthyReceiver, "inproc://perf-paced-healthy", 4_096_000);

List<ISocket> senders = [blocked, healthy];
_ = poller.PollSockets(senders, PollEventFlags.PollCompletion, 0);
using var cancelProbe = new CancellationTokenSource();
int queued = 0;
bool backpressured = false;
while (queued < 64)
{
    using Message filler = Message.Allocate(Size);
    filler.AsSpan().Clear();
    SendSubmission probe = blocked.Send().Message(filler).Async(cancelProbe.Token);
    if (probe.Result == SubmitResult.Backpressured)
    {
        backpressured = !probe.Admitted.IsCompleted;
        cancelProbe.Cancel();
        try
        {
            probe.Admitted.WaitAsync(testTimeout).GetAwaiter().GetResult();
        }
        catch (OperationCanceledException) when (cancelProbe.IsCancellationRequested)
        {
        }
        break;
    }
    queued++;
}
if (!backpressured || queued == 0)
    throw new InvalidOperationException("Could not fill the blocked sender's pipe.");

using var ready = new ManualResetEventSlim();
using var stopped = new CancellationTokenSource();
int healthyWhileBlocked = 0;
int resumed = 0;
Task receiver = Task.Factory.StartNew(() =>
{
    var received = Received.Create();
    ready.Set();
    while (healthyWhileBlocked < ExpectedHealthy && Receive(healthyReceiver, received))
        healthyWhileBlocked++;

    // The pipe stays full until the fourth healthy admission, or until
    // the test times out. Drain on timeout as well so the sender can exit.
    var blockedReceived = Received.Create();
    for (int i = 0; i < queued; i++)
        if (!Receive(blockedReceiver, blockedReceived))
            throw new InvalidOperationException("A prefilled message was lost.");
    if (Receive(blockedReceiver, blockedReceived))
        resumed = 1;
    while (!stopped.IsCancellationRequested)
    {
        if (!blockedReceiver.Recv(blockedReceived, RecvFlags.DontWait))
            Thread.Yield();
    }
}, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);

ready.Wait();
var schedule = new MultiPacedSchedule(20_000);
long seq = 0;
try
{
    PerfMultiDealerDealerClient.RunSendWindow(poller, senders,
        Size, 1, schedule, schedule.EpochNs + 250_000_000, () => false, ref seq);
}
finally
{
    stopped.Cancel();
    receiver.GetAwaiter().GetResult();
}
Console.WriteLine($"backpressured={backpressured} healthy_while_blocked={healthyWhileBlocked} "
    + $"resumed={resumed} accepted={schedule.AcceptedCount}");
return healthyWhileBlocked == ExpectedHealthy && resumed == 1
    && schedule.AcceptedCount >= ExpectedHealthy + 1 ? 0 : 1;

void Connect(IDealerSocket sender, IDealerSocket receiver, string endpoint, ulong hwm)
{
    sender.Options.Linger = TimeSpan.Zero;
    receiver.Options.Linger = TimeSpan.Zero;
    sender.Options.SendHighWaterMark = hwm;
    sender.Options.SendTimeout = testTimeout;
    receiver.Options.ReceiveHighWaterMark = hwm;
    receiver.Bind(endpoint);
    sender.Connect(endpoint);
    using Message probe = Message.Allocate(1);
    sender.Send().Message(probe).Submit();
    if (!Receive(receiver, Received.Create()))
        throw new InvalidOperationException("An inproc pair did not become ready.");
}

bool Receive(IDealerSocket socket, Received message)
    => SpinWait.SpinUntil(() => socket.Recv(message, RecvFlags.DontWait),
        testTimeout);

static void CheckRotation()
{
    var rotation = new MultiSenderRotation(3);
    for (int sender = 0; sender < 3; sender++)
    {
        for (int send = 1; send <= 64; send++)
        {
            Require(rotation.Index == sender, "rotation left the sender early");
            Require(rotation.Accepted() == (send == 64), "rotation did not yield at 64");
        }
    }

    Require(rotation.Index == 0, "rotation did not wrap");
    _ = rotation.Accepted();
    rotation.Advance();
    for (int send = 1; send < 64; send++)
        Require(!rotation.Accepted(), "rotation carried a blocked sender's count forward");
    Require(rotation.Accepted() && rotation.Index == 2, "rotation did not reset on yield");
    Console.WriteLine("PASS 1 sender rotation at 64, wrap, and blocked yield");
}

static void CheckReplyTurn()
{
    int submissions = 0;
    var turn = new MultiPacedReplyTurn(() => submissions++);
    for (int reply = 1; reply <= 129; reply++)
    {
        turn.Received();
        Require(submissions == reply / 64, "paced submission did not run at the reply boundary");
    }
    Console.WriteLine("PASS 4 paced submission on replies 64 and 128");
}

static void CheckCatchUp()
{
    const ulong start = 1_000_000_000;
    var tracker = new PerfMultiPubSubClient.CatchUpTracker(2);
    Require(!tracker.Observe(start, [90, 100], 100, 20.0), "initial observation caught up");
    Require(!tracker.Observe(start + 500_000_000, [150, 140], 150, 20.0),
        "a change in the slowest subscriber concealed its deficit");
    Require(tracker.Observe(start + 1_000_000_000, [199, 190], 200, 20.0),
        "one message of observation quantization blocked catch-up");

    tracker = new PerfMultiPubSubClient.CatchUpTracker(2);
    Require(!tracker.Observe(start, [90, 100], 100, 20.0), "initial observation caught up");
    Require(!tracker.Observe(start + 200_000_000, [100, 140], 120, 20.0),
        "in-flight growth caught up");
    Require(!tracker.Observe(start + 500_000_000, [140, 160], 150, 20.0),
        "window was not restarted after in-flight growth");
    Require(tracker.Observe(start + 700_000_000, [150, 190], 170, 20.0)
        && tracker.WindowStartInFlight == 20, "stable nonzero in-flight did not catch up");
    Console.WriteLine("PASS 5 lagging subscriber, in-flight restart, and one-message tolerance");
}

static void CheckSchedule()
{
    var schedule = new MultiPacedSchedule(100);
    ulong epoch = schedule.EpochNs;
    Require(schedule.Due(epoch) && schedule.Submitted() == epoch, "initial send missed the epoch");
    ulong next = epoch + 10_000_000;
    Require(!schedule.Due(next - 1) && schedule.Due(next), "shared interval drifted");
    Require(schedule.WaitMilliseconds(epoch + 8_000_001, next) == 1, "wait was not floored");
    Require(schedule.WaitMilliseconds(epoch + 9_000_001, next) == 0, "sub-ms wait did not recheck");
    Require(MultiPacedSchedule.MillisecondsUntil(epoch, epoch + 51_000_000) == 50,
        "wait exceeded 50 ms");
    Console.WriteLine("PASS 2/3 shared epoch and wait rounding");
}

static void Require(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}
