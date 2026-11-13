"""Arithmetic for the proposed design. All ranges here are hypotheses, not measurements."""
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def plus(*ranges): return [sum(r[0] for r in ranges),sum(r[1] for r in ranges)]
def scale(r,k): return [v*k for v in r]

# Each row belongs to exactly one sum. See report for clauses and primitive counts.
S={'S1_call_codec_plan':[.10,.20], 'S2_logical_admission_lane_port':[.12,.30],
   'S3_repeatable_header':[.09,.12], 'S4_service_wire':[.30,.45],
   'S5_value_ownership_result':[.08,.16], 'S6_diagnostics':[.02,.04]}
R={'R1_ingress_classification':[.18,.30], 'R2_permit_commit':[.10,.18],
   'R3_take_ready':[.10,.18], 'R4_part_views':[.08,.14],
   'R5_header_hit':[.07,.10], 'R6_route_context':[.08,.16],
   'R7_fresh_scope_handler':[.18,.35], 'R8_invoker_coroutine':[.18,.46],
   'R9_terminal_release':[.08,.16], 'R10_diagnostics':[.02,.04]}
Q={'Q1_call_codec_plan':[.10,.20], 'Q2_logical_admission_lane_port':[.12,.30],
   'Q3_dynamic_header':[.35,.60], 'Q4_service_wire':[.30,.45],
   'Q5_logical_pending_deadline_slot':[.30,.55],
   'Q6_call_and_final_result_storage':[.20,.35],
   'Q7_binding_observer_coroutine':[.18,.46], 'Q8_value_projection':[.08,.16],
   'Q9_diagnostics':[.02,.04]}
C={'C1_reply_json_header':[.65,1.00], 'C2_wire_part_views':[.08,.14],
   'C3_terminal_take':[.10,.20], 'C4_final_result_publication':[.10,.20],
   'C5_independent_dispatcher_item':[.10,.20]}
RB={'management':[.30,.80], 'worker_publish_notify':[.80,2.00],
    'owner_drain_coroutine':[.18,.46]}
CB=[.80,2.00]
SP=[.80,1.56]  # pending send: entry/result .50-.90, frame .18-.46, take/item .12-.20
QP=[.10,.20]  # extra admission suspension/resume on the same request observer
COPY_RATE_BYTES_PER_US=16000  # explicit scenario, not a measured machine property

cells=json.loads((ROOT/'analysis/recomputed.json').read_text())['cells']
rows=[]
for cell in cells:
    for batch in [1,4,8,64]:
        for pending in [0,1]:
            copy_extra=(cell['size']-1024)/COPY_RATE_BYTES_PER_US
            source_send=plus(*S.values(),[copy_extra,copy_extra])
            receive=plus(*R.values(),scale(plus(*RB.values()),1/batch))
            source_request_submit=plus(*Q.values(),[copy_extra,copy_extra],scale(QP,pending))
            complete=plus(*C.values(),scale(CB,1/batch))
            target_request=plus(receive,[-R['R5_header_hit'][0],-R['R5_header_hit'][1]],
                                C['C1_reply_json_header'],[.18,.46], # keep typed adapter coroutine
                                Q['Q3_dynamic_header'],S['S4_service_wire'],
                                [copy_extra,copy_extra])
            if cell['pattern']=='send-saturation':
                source=plus(source_send,scale(plus(SP,scale(CB,1/batch)),pending))
                # Charge the full pending coroutine/entry amount to the caller
                # conservatively; do not hide creation on the submit thread.
                caller=plus(source_send,scale(SP,pending))
                target=receive
            else:
                source=plus(source_request_submit,complete)
                caller=source_request_submit
                target=target_request
            row=dict(pattern=cell['pattern'],size=cell['size'],batch=batch,pending_fraction=pending,
                     source_extra_us=source,target_extra_us=target,caller_extra_us=caller,
                     source_cpu_margin_upper_us=cell['source_cpu_effective_share_us']-source[1],
                     target_cpu_margin_upper_us=cell['target_cpu_effective_share_us']-target[1],
                     caller_margin_upper_us=cell['caller_effective_share_us']-caller[1])
            if cell['pattern']=='request-serial':
                # Sum extra endpoint CPU as a conservative serialized-path scenario;
                # not a claim that baseline process CPU equals baseline RTT.
                row['extra_path_cpu_scenario_us']=plus(source,target)
                row['unpriced_wait_and_native_headroom_us']=(cell['wall_effective_share_us']
                                                           - source[1]-target[1])
            rows.append(row)
data=dict(assumptions={'cycles_per_us':3000,'simple_instructions_per_cycle':1,
                      'atomic_rmw_ns':[15,50],'uncontended_mutex_pair_ns':[30,80],
                      'pool_acquire_release_ns':[80,160],
                      'coroutine_frame_lifecycle_rmw':[3,5],
                      'coroutine_frame_lifecycle_simple_instructions':150,
                      'copy_bytes_per_us':COPY_RATE_BYTES_PER_US,
                      'batch_is_records_per_actual_notification_not_capacity':True,
                      'native_cpu_change_not_estimated':True,
                      'os_queue_delay_not_estimated':True},
          operations={'S':S,'R':R,'Q':Q,'C':C,'receive_batch':RB,
                      'completion_batch':CB,'pending_send_extra':SP,'pending_request_extra':QP},
          cases=rows)
(ROOT/'analysis/paper-sums.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
for r in rows:
    if r['batch']==8 and r['pending_fraction']==0 or r['batch']==1 and r['pending_fraction']==1 and r['pattern']=='request-serial':
        print(r)
