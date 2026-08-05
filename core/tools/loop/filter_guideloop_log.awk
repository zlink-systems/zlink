BEGIN {
  in_exec_body = 0
  exec_body_lines = 0
  exec_body_truncated = 0
  in_diff = 0
}

function is_control_line(line) {
  return line ~ /^(OpenAI Codex|--------|user|codex|exec|warning:|=== guideloop iteration|guide: |progress: |source spec: |log: |guide status:|guide remaining_tasks:|guide completion_verified:|guide blocked_reason:|Guide loop|Max iterations reached|codex exec failed|Guide loop requested user input|[0-9]{4}-[0-9]{2}-[0-9]{2}T)/
}

function flush_exec_truncation() {
  if (exec_body_truncated) {
    print "[exec output truncated]"
  }
  exec_body_truncated = 0
  exec_body_lines = 0
  in_exec_body = 0
}

{
  line = $0

  if (in_diff) {
    if (is_control_line(line)) {
      in_diff = 0
    } else {
      next
    }
  }

  if (in_exec_body) {
    if (is_control_line(line)) {
      flush_exec_truncation()
    } else {
      exec_body_lines++
      if (exec_body_lines <= 12) {
        print line
      } else {
        exec_body_truncated = 1
      }
      next
    }
  }

  if (line ~ /^diff --git /) {
    print "[diff output omitted]"
    in_diff = 1
    next
  }

  print line

  if (line ~ /^ succeeded in .*:$/ || line ~ /^ exited [0-9]+ in .*:$/) {
    in_exec_body = 1
    exec_body_lines = 0
    exec_body_truncated = 0
  }
}

END {
  if (in_exec_body && exec_body_truncated) {
    print "[exec output truncated]"
  }
}
