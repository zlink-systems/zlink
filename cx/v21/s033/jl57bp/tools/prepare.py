from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/jl57bp')
p=job/'tools/java/run_local_binding.sh'
s=p.read_text().replace('/root/jobs/fx/cx/v21/s033/lad-java-s3/tools/java',str(job/'tools/java'))
s=s.replace('printf \'%s_tid=%s\\n\' "$label" "$tid" >> "$BINDING_SCHED_FILE"', '''printf '%s_tid=%s\\n' "$label" "$tid" >> "$BINDING_SCHED_FILE"
      task_affinity="$(awk '/^Cpus_allowed_list:/ {print $2}' "$task/status" 2>/dev/null || true)"
      printf '%s_task_affinity tid=%s cpus=%s\\n' "$label" "$tid" "${task_affinity:-unavailable}" >> "$BINDING_SCHED_FILE"''')
p.write_text(s)
p=job/'tools/java/runner_common.sh'
s=p.read_text().replace('  trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" warmup', '''  python3 /root/jobs/fx/cx/v21/s033/jl57bp/tools/sample-stats.py "${source_url}" "${target_url}" "${result_file%/*}/stats-timeline.jsonl" &
  local stats_sampler_pid=$!
  trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" warmup''')
s=s.replace('  verify_request_counts "${result_file}"','  kill "${stats_sampler_pid}" 2>/dev/null || true\n  wait "${stats_sampler_pid}" 2>/dev/null || true\n  verify_request_counts "${result_file}"')
p.write_text(s)
