mod dealer;
mod pair;
mod pub_socket;
mod router;
mod stream;
mod sub;
mod xpub;
mod xsub;

pub(crate) use crate::internal::SocketStorage as SocketInner;
pub(crate) use dealer::dealer_inner;
pub(crate) use pair::{pair_handle, pair_inner};
pub(crate) use pub_socket::pub_inner;
pub(crate) use router::router_inner;
pub(crate) use stream::stream_inner;
pub(crate) use sub::sub_inner;
pub(crate) use xpub::xpub_inner;
pub(crate) use xsub::xsub_inner;

use std::ffi::{CStr, CString, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::time::Duration;

use crate::contracts::errors::errors::{
    BindError, CloseError, ConfigError, ConnectError, RecvError, RecvResult,
};
use crate::contracts::messaging::message::{Message, RoutingId};
use crate::contracts::messaging::received::Received;
use crate::contracts::messaging::subscription_event::SubscriptionEvent;
use crate::contracts::messaging::topic_message::TopicMessage;
use crate::contracts::sockets::socket_options::RecvFlags;
use crate::runtime::errors::native_errors::{
    check_bind_rc, check_close_rc, check_config_rc, check_connect_rc, check_recv_rc,
    config_validation_error, last_errno,
};
use crate::runtime::handles::ctx::{context_handle, duration_to_millis};
use crate::runtime::native::ffi;

mod socket_inner_runtime;
mod socket_parts_runtime;
pub(crate) use socket_parts_runtime::*;
mod socket_options_runtime;
use socket_options_runtime::*;
