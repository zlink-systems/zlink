// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink;
using static PerfRunner;

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
    PerfMultiDealerDealerClient.RunPacedWindow(poller, senders,
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
