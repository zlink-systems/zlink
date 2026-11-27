use super::SocketInner;
use crate::contracts::errors::errors::{ConfigError, ConfigResult};
use crate::contracts::sockets::connection_attempt::{
    ConnectionAttempt, ConnectionAttemptState, ConnectionFailure, ConnectionFailureKind,
    ConnectionFailureStage,
};
use crate::runtime::errors::native_errors::check_config_rc;
use crate::runtime::native::ffi;

/// Core returned a row or enum value the closed contract cannot represent.
fn protocol_violation() -> ConfigError {
    ConfigError::new(ConfigResult::InternalError, 0)
}

fn attempt_state(raw: u32) -> Result<ConnectionAttemptState, ConfigError> {
    Ok(match raw {
        0 => ConnectionAttemptState::Connecting,
        1 => ConnectionAttemptState::Connected,
        2 => ConnectionAttemptState::AttemptFailed,
        _ => return Err(protocol_violation()),
    })
}

fn failure_kind(raw: u32) -> Result<ConnectionFailureKind, ConfigError> {
    Ok(match raw {
        0 => ConnectionFailureKind::None,
        1 => ConnectionFailureKind::Refused,
        2 => ConnectionFailureKind::EndpointMissing,
        3 => ConnectionFailureKind::TimedOut,
        4 => ConnectionFailureKind::Unreachable,
        5 => ConnectionFailureKind::NameResolution,
        6 => ConnectionFailureKind::AuthFailed,
        7 => ConnectionFailureKind::ProtocolFailed,
        8 => ConnectionFailureKind::ConnectionLost,
        9 => ConnectionFailureKind::Other,
        _ => return Err(protocol_violation()),
    })
}

fn failure_stage(raw: u32) -> Result<ConnectionFailureStage, ConfigError> {
    Ok(match raw {
        0 => ConnectionFailureStage::None,
        1 => ConnectionFailureStage::Resolve,
        2 => ConnectionFailureStage::Transport,
        3 => ConnectionFailureStage::Tls,
        4 => ConnectionFailureStage::WebSocket,
        5 => ConnectionFailureStage::CoreHandshake,
        6 => ConnectionFailureStage::Established,
        _ => return Err(protocol_violation()),
    })
}

impl SocketInner {
    /// Reads the connection-attempts snapshot. BUFFER_TOO_SMALL reports the
    /// required row and endpoint byte counts and keeps POLLCONNECT readiness,
    /// so retry with those capacities.
    pub(crate) fn connection_attempts_snapshot(
        &self,
    ) -> Result<Vec<ConnectionAttempt>, ConfigError> {
        let mut rows: Vec<ffi::zlink_connection_attempt_t> = Vec::new();
        let mut bytes: Vec<u8> = Vec::new();
        loop {
            let mut count = 0usize;
            let mut required = 0usize;
            let rc = unsafe {
                ffi::zlink_socket_connection_attempts_snapshot(
                    self.handle,
                    rows.as_mut_ptr(),
                    rows.len(),
                    &mut count,
                    bytes.as_mut_ptr().cast(),
                    bytes.len(),
                    &mut required,
                )
            };
            if rc == ffi::zlink_config_result_t::ZLINK_CONFIG_BUFFER_TOO_SMALL as i32 {
                if count <= rows.len() && required <= bytes.len() {
                    return Err(protocol_violation());
                }
                let empty = ffi::zlink_connection_attempt_t {
                    endpoint_offset: 0,
                    endpoint_length: 0,
                    intent_id: 0,
                    revision: 0,
                    last_failure_id: 0,
                    state: 0,
                    last_failure: ffi::zlink_connection_failure_t {
                        kind: 0,
                        stage: 0,
                        normalized_errno: 0,
                        disconnect_reason: 0,
                    },
                };
                rows.resize(rows.len().max(count), empty);
                bytes.resize(bytes.len().max(required), 0);
                continue;
            }
            check_config_rc(rc)?;
            if count > rows.len() || required > bytes.len() {
                return Err(protocol_violation());
            }
            return rows[..count]
                .iter()
                .map(|row| {
                    let end = row
                        .endpoint_offset
                        .checked_add(row.endpoint_length)
                        .ok_or_else(protocol_violation)?;
                    if row.intent_id == 0 || row.revision == 0 || end >= required || bytes[end] != 0
                    {
                        return Err(protocol_violation());
                    }
                    let endpoint = std::str::from_utf8(&bytes[row.endpoint_offset..end])
                        .map_err(|_| protocol_violation())?
                        .to_owned();
                    Ok(ConnectionAttempt {
                        endpoint,
                        intent_id: row.intent_id,
                        revision: row.revision,
                        last_failure_id: row.last_failure_id,
                        state: attempt_state(row.state)?,
                        last_failure: ConnectionFailure {
                            kind: failure_kind(row.last_failure.kind)?,
                            stage: failure_stage(row.last_failure.stage)?,
                            normalized_errno: row.last_failure.normalized_errno,
                            disconnect_reason: row.last_failure.disconnect_reason,
                        },
                    })
                })
                .collect();
        }
    }
}
