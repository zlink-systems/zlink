use std::time::Duration;

use crate::error::ConfigError;
use crate::ffi;
use crate::internal::SocketStorage;
use crate::message::{Message, RoutingId};
use crate::socket_contracts::DealerSocket;

impl SocketStorage {
    pub(crate) fn set_mandatory(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_router_bool_opt(
            ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_MANDATORY,
            enabled,
        )
    }
    pub(crate) fn set_probe(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_router_bool_opt(ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_PROBE, enabled)
    }
    pub(crate) fn set_connect_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        self.set_router_bytes_opt(
            ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
            id.data(),
        )
    }
    pub(crate) fn weight(&self) -> Result<u32, ConfigError> {
        self.get_router_u32_opt(ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_WEIGHT)
    }
    pub(crate) fn set_weight(&self, value: u32) -> Result<(), ConfigError> {
        self.set_router_u32_opt(ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_WEIGHT, value)
    }
    pub(crate) fn request_timeout(&self) -> Result<Duration, ConfigError> {
        let ms = self
            .get_router_i32_opt(ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS)?;
        Ok(Duration::from_millis(ms as u64))
    }
    pub(crate) fn set_request_timeout(&self, value: Duration) -> Result<(), ConfigError> {
        let millis = value.as_millis().min(i32::MAX as u128) as i32;
        self.set_router_i32_opt(
            ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS,
            millis,
        )
    }
}

impl DealerSocket {
    pub(crate) fn set_probe(&self, enabled: bool) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self)
            .set_dealer_bool_opt(ffi::zlink_dealer_option_t::ZLINK_DEALER_OPT_PROBE, enabled)
    }
    pub(crate) fn weight(&self) -> Result<u32, ConfigError> {
        crate::socket::dealer_inner(self)
            .get_dealer_u32_opt(ffi::zlink_dealer_option_t::ZLINK_DEALER_OPT_WEIGHT)
    }
    pub(crate) fn set_weight(&self, value: u32) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self)
            .set_dealer_u32_opt(ffi::zlink_dealer_option_t::ZLINK_DEALER_OPT_WEIGHT, value)
    }
    pub(crate) fn set_request_timeout(&self, value: Duration) -> Result<(), ConfigError> {
        let millis = value.as_millis().min(i32::MAX as u128) as i32;
        crate::socket::dealer_inner(self).set_dealer_i32_opt(
            ffi::zlink_dealer_option_t::ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS,
            millis,
        )
    }
}

impl SocketStorage {
    pub(crate) fn set_notify(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_stream_bool_opt(ffi::zlink_stream_option_t::ZLINK_STREAM_OPT_NOTIFY, enabled)
    }
    pub(crate) fn notify(&self) -> Result<bool, ConfigError> {
        self.get_stream_bool_opt(ffi::zlink_stream_option_t::ZLINK_STREAM_OPT_NOTIFY)
    }
}

impl SocketStorage {
    pub(crate) fn set_verbose(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_pub_bool_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_VERBOSE, enabled)
    }
    pub(crate) fn set_verboser(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_pub_bool_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_VERBOSER, enabled)
    }
    pub(crate) fn set_no_drop(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_pub_bool_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_NODROP, enabled)
    }
    pub(crate) fn set_manual(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_pub_bool_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_MANUAL, enabled)
    }
    pub(crate) fn manual_last_value(&self) -> Result<bool, ConfigError> {
        self.get_pub_bool_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_MANUAL_LAST_VALUE)
    }
    pub(crate) fn set_manual_last_value(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_pub_bool_opt(
            ffi::zlink_pub_option_t::ZLINK_PUB_OPT_MANUAL_LAST_VALUE,
            enabled,
        )
    }
    pub(crate) fn welcome_message(&self) -> Result<Message, ConfigError> {
        self.get_pub_message_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_WELCOME_MSG)
    }
    pub(crate) fn set_welcome_message(&self, message: &Message) -> Result<(), ConfigError> {
        self.set_pub_bytes_opt(
            ffi::zlink_pub_option_t::ZLINK_PUB_OPT_WELCOME_MSG,
            message.as_bytes(),
        )
    }
    pub(crate) fn approve_subscribe(&self, routing_id: &RoutingId) -> Result<(), ConfigError> {
        self.set_pub_bytes_opt(
            ffi::zlink_pub_option_t::ZLINK_PUB_OPT_APPROVE_SUBSCRIBE,
            routing_id.data(),
        )
    }
    pub(crate) fn reject_subscribe(&self, routing_id: &RoutingId) -> Result<(), ConfigError> {
        self.set_pub_bytes_opt(
            ffi::zlink_pub_option_t::ZLINK_PUB_OPT_REJECT_SUBSCRIBE,
            routing_id.data(),
        )
    }
    pub(crate) fn pub_topics_count(&self) -> Result<i32, ConfigError> {
        self.get_pub_int_opt(ffi::zlink_pub_option_t::ZLINK_PUB_OPT_TOPICS_COUNT)
    }
}

impl SocketStorage {
    pub(crate) fn sub_topics_count(&self) -> Result<i32, ConfigError> {
        self.get_sub_int_opt(ffi::zlink_sub_option_t::ZLINK_SUB_OPT_TOPICS_COUNT)
    }
}
