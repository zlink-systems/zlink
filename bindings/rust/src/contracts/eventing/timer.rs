use std::sync::Arc;

use crate::internal::TimerStorage;

/// A timer that fires on an interval and can be polled or awaited.
pub struct Timer {
    pub(crate) inner: Arc<TimerStorage>,
}
