"""Process failure details shared by the C benchmark runners."""


def first_stderr_error(stderr_text, max_len=180):
    lines = [line.strip() for line in (stderr_text or "").splitlines() if line.strip()]
    if not lines:
        return ""
    line = next(
        (line for line in lines if any(word in line.lower() for word in
         ("error", "failed", "failure", "errno", "timeout"))),
        lines[0],
    )
    return line.replace("\r", " ").replace("\n", " ")[:max_len]


def process_failure_reason(client_rc, client_stderr, server_rc=None, server_stderr=""):
    parts = [f"client_exit={client_rc}"]
    client_error = first_stderr_error(client_stderr)
    if client_error:
        parts.append(f"client_stderr={client_error}")
    if server_rc is not None:
        parts.append(f"server_exit={server_rc}")
        server_error = first_stderr_error(server_stderr)
        if server_error:
            parts.append(f"server_stderr={server_error}")
    return "; ".join(parts)
