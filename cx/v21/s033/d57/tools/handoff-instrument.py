from pathlib import Path
import shutil,difflib,sys
root=Path('/root/worktree/zlink-v21-s033/d57-scratch/framework/languages/dotnet/src/Zlink.Framework')
raw=Path('/root/jobs/fx/cx/v21/s033/d57/raw/handoff-instrumentation')
files=['Runtime/Service/ZLinkManagedMeshNode.cs','Runtime/Service/FrameworkServiceTypes.cs','Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs','Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs','Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs']
helper=root/'D57Handoffs.cs'
if sys.argv[1]=='restore':
 for f in files:shutil.copy2(raw/'pristine'/f,root/f)
 helper.unlink()
 sys.exit()
raw.mkdir(parents=True,exist_ok=False)
for f in files:
 dest=raw/'pristine'/f;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/f,dest)
helper.write_text("""using System.Diagnostics.Tracing;
namespace Zlink.Framework;
[EventSource(Name = "D57-Handoffs")]
internal sealed class D57Handoffs : EventSource
{
    internal static readonly D57Handoffs Log = new();
    [Event(1, Level = EventLevel.Informational)]
    public void Transition(int stage, int previous, int current, long operation) =>
        WriteEvent(1, stage, previous, current, operation);
}
""")
p=root/files[0];s=p.read_text();idx=s.index('    private async ValueTask<TReply> RequestNativeDurableWireAsync<TReply>(');idx=s.index('    {',idx)+6;s=s[:idx]+"        var profileCallerThread = Environment.CurrentManagedThreadId;\n"+s[idx:]
needle='                        var route = ObservePeerRouteUnderOwner(\n                            target,\n                            targetNodeGeneration,\n                            out var peer\n                        );'
assert needle in s
s=s.replace(needle,"                        var profileSelectorThread = Environment.CurrentManagedThreadId;\n                        D57Handoffs.Log.Transition(1, profileCallerThread, profileSelectorThread, (long)operation.OperationId.Low);\n"+needle,1)
needle='                            start: pending =>\n                            {\n                                var frames = Interlocked.Exchange(ref ownedWire, null)!;'
assert needle in s
s=s.replace(needle,needle.replace('                                var frames', '                                D57Handoffs.Log.Transition(2, profileSelectorThread, Environment.CurrentManagedThreadId, (long)pending.OperationId.Low);\n                                var frames'),1)
s=s.replace('        internal RoutingId LogicalTarget { get; set; }','        internal int ProfileTerminalThread;\n        internal RoutingId LogicalTarget { get; set; }',1)
s=s.replace('            ZLinkCompletionDispatcher.Post(this);','            ProfileTerminalThread = Environment.CurrentManagedThreadId;\n            ZLinkCompletionDispatcher.Post(this);')
s=s.replace('            var failure = _completionFailure;','            D57Handoffs.Log.Transition(3, ProfileTerminalThread, Environment.CurrentManagedThreadId, (long)OperationId.Low);\n            var failure = _completionFailure;',1)
needle='        var payloadBytes = GetApplicationPayloadBytes(record, parts);\n        record.ApplicationPayloadBytes = payloadBytes;'
assert needle in s
s=s.replace(needle,'        record.ProfileReceiveThread = Environment.CurrentManagedThreadId;\n'+needle,1);p.write_text(s)
p=root/files[1];s=p.read_text().replace('internal struct MeshReceiveRecord\n{','internal struct MeshReceiveRecord\n{\n    internal int ProfileReceiveThread;',1);p.write_text(s)
p=root/files[2];s=p.read_text();idx=s.index('    private void EnqueueNodeRoute(');idx=s.index('    {',idx)+6;s=s[:idx]+'        D57Handoffs.Log.Transition(4, record.ProfileReceiveThread, Environment.CurrentManagedThreadId, (long)record.OperationId.Low);\n'+s[idx:];p.write_text(s)
p=root/files[3];s=p.read_text().replace('        private Exception? _failure;','        private Exception? _failure;\n        private int _profileAdmissionThread;',1).replace('        internal void PrepareSend(Exception? failure) => _failure = failure;','        internal void PrepareSend(Exception? failure) { _profileAdmissionThread = Environment.CurrentManagedThreadId; _failure = failure; }',1)
idx=s.index('        internal override void Execute()',s.index('internal sealed class PendingSend'));idx=s.index('        {',idx)+10;s=s[:idx]+'            D57Handoffs.Log.Transition(7, _profileAdmissionThread, Environment.CurrentManagedThreadId, (long)CorrelationId.Low);\n'+s[idx:];p.write_text(s)
p=root/files[4];s=p.read_text();idx=s.index('    private async ValueTask DispatchAsync(');idx=s.index('    {',idx)+6;s=s[:idx]+'        var profileWorkerThread = Environment.CurrentManagedThreadId;\n'+s[idx:];needle='            using (admission.Lease)';assert needle in s;s=s.replace(needle,'            D57Handoffs.Log.Transition(5, profileWorkerThread, Environment.CurrentManagedThreadId, (long)received.OperationId.Low);\n'+needle,1);p.write_text(s)
for f in files:
 before=(raw/'pristine'/f).read_text();after=(root/f).read_text();dest=raw/'instrumented'/f;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(after)
 (raw/(Path(f).name+'.diff')).write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile=f,tofile=f)))
shutil.copy2(helper,raw/'D57Handoffs.cs')
