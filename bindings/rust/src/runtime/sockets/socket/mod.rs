mod dealer;
mod pair;
mod pub_socket;
mod router;
mod stream;
mod sub;
mod xpub;
mod xsub;

pub(crate) use crate::internal::{CallbackBox, SocketStorage as SocketInner};
pub(crate) use dealer::{dealer_inner, dealer_inner_mut};
pub(crate) use pair::{pair_handle, pair_inner, pair_inner_mut};
pub(crate) use pub_socket::{pub_inner, pub_inner_mut};
pub(crate) use router::{recv_router_once, router_inner, router_inner_mut};
pub(crate) use stream::{stream_inner, stream_inner_mut, stream_on_packet};
pub(crate) use sub::{sub_inner, sub_inner_mut};
pub(crate) use xpub::{xpub_inner, xpub_inner_mut};
pub(crate) use xsub::{xsub_inner, xsub_inner_mut};

use std::ffi::{CStr, CString, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::time::Duration;

use crate::ctx::{context_handle, duration_to_millis};
use crate::domain::Received;
use crate::error::{
    BindError, CloseError, ConfigError, ConnectError, HandlerError, RecvError, RecvResult,
    SubmitError,
};
use crate::ffi;
use crate::flags::RecvFlags;
use crate::message::{Message, RoutingId};
use crate::messaging_subscription_event::SubscriptionEvent;
use crate::native_errors::{
    check_bind_rc, check_close_rc, check_config_rc, check_connect_rc, check_handler_rc,
    check_recv_rc, config_validation_error, last_errno, submit_validation_error,
};
use crate::topic_message_contract::TopicMessage;

mod socket_inner_runtime;
mod socket_parts_runtime;
pub(crate) use socket_parts_runtime::*;
mod socket_options_runtime;
use socket_options_runtime::*;
