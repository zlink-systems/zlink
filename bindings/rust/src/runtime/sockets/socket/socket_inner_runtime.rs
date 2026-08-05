use super::*;
impl crate::internal::SocketStorage {
    pub(crate) fn create(
        ctx: &crate::core_context::Context,
        typ: ffi::zlink_socket_type_t,
    ) -> Result<Self, ConfigError> {
        let handle = unsafe { ffi::zlink_socket(context_handle(ctx), typ) };
        if handle.is_null() {
            return Err(ConfigError::new(
                crate::error::ConfigResult::InvalidHandle,
                last_errno(),
            ));
        }
        Ok(Self {
            handle,
            send_ready_cb: None,
            packet_cb: None,
        })
    }

    // -- Connection --------------------------------------------------------

    pub(crate) fn bind(&self, addr: &str) -> Result<(), BindError> {
        let c = CString::new(addr)
            .map_err(|_| BindError::new(crate::error::BindResult::InvalidArgument, libc::EINVAL))?;
        check_bind_rc(unsafe { ffi::zlink_bind(self.handle, c.as_ptr()) })
    }

    pub(crate) fn connect(&self, addr: &str) -> Result<(), ConnectError> {
        let c = CString::new(addr).map_err(|_| {
            ConnectError::new(crate::error::ConnectResult::InvalidArgument, libc::EINVAL)
        })?;
        check_connect_rc(unsafe { ffi::zlink_connect(self.handle, c.as_ptr()) })
    }

    pub(crate) fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
        let c = CString::new(addr).map_err(|_| {
            ConnectError::new(crate::error::ConnectResult::InvalidArgument, libc::EINVAL)
        })?;
        check_connect_rc(unsafe { ffi::zlink_unbind(self.handle, c.as_ptr()) })
    }

    pub(crate) fn disconnect(&self, addr: &str) -> Result<(), ConnectError> {
        let c = CString::new(addr).map_err(|_| {
            ConnectError::new(crate::error::ConnectResult::InvalidArgument, libc::EINVAL)
        })?;
        check_connect_rc(unsafe { ffi::zlink_disconnect(self.handle, c.as_ptr()) })
    }

    pub(crate) fn disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError> {
        check_connect_rc(unsafe { ffi::zlink_disconnect_rid(self.handle, peer_rid.as_raw()) })
    }

    // -- Recv (direct) -----------------------------------------------------

    /// Receives a message into the [`Received`] object supplied by the caller.
    /// Passing the same [`Received`] repeatedly lets the binding refill its
    /// internal state in place after each successful call.
    ///
    /// Returns `Ok(true)` on success, `Ok(false)` when [`RecvFlags::DONT_WAIT`]
    /// finds no data, and `Err(_)` on hard error. See
    /// `doc/spec/bindings/README.md` for the caller-provided receive shape.
    pub(crate) fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        let routing_id = recv_basic_parts(self.handle, flags.bits(), out.receive_scratch())?;
        match routing_id {
            Some(routing_id) => {
                out.replace_received_parts(routing_id);
                Ok(true)
            }
            None => Ok(false),
        }
    }

    // -- Subscribe (blocking recv) -----------------------------------------

    pub(crate) fn subscribe_recv(
        &self,
        out: &mut TopicMessage,
        flags: RecvFlags,
    ) -> Result<bool, RecvError> {
        let mut topic_buf = [0i8; 256];
        let received = recv_subscribed_parts(
            self.handle,
            &mut topic_buf,
            flags.bits(),
            out.receive_scratch(),
        )?;
        match received {
            Some((routing_id, topic)) => {
                out.replace_received_parts(routing_id, topic);
                Ok(true)
            }
            None => Ok(false),
        }
    }

    // -- Subscription management -------------------------------------------

    pub(crate) fn set_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        let c = CString::new(filter).map_err(|_| config_validation_error())?;
        check_config_rc(unsafe { ffi::zlink_set_subscription(self.handle, c.as_ptr()) })
    }

    pub(crate) fn unset_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        let c = CString::new(filter).map_err(|_| config_validation_error())?;
        check_config_rc(unsafe { ffi::zlink_unset_subscription(self.handle, c.as_ptr()) })
    }

    pub(crate) fn subscription_at(
        &self,
        index: usize,
    ) -> Result<Option<(String, bool)>, ConfigError> {
        let mut len = 0usize;
        let mut is_pattern = 0i32;
        let rc = unsafe {
            ffi::zlink_subscription_at(
                self.handle,
                index,
                ptr::null_mut(),
                &mut len,
                &mut is_pattern,
            )
        };
        if rc != 0 && len == 0 {
            let err = check_config_rc(rc).expect_err("nonzero subscription_at result");
            if err.code() == crate::error::ConfigResult::NotFound {
                return Ok(None);
            }
            return Err(err);
        }
        if len == 0 {
            check_config_rc(unsafe {
                ffi::zlink_subscription_at(
                    self.handle,
                    index,
                    ptr::null_mut(),
                    &mut len,
                    &mut is_pattern,
                )
            })?;
            return Ok(Some((String::new(), is_pattern != 0)));
        }
        let mut buffer = vec![0i8; len];
        check_config_rc(unsafe {
            ffi::zlink_subscription_at(
                self.handle,
                index,
                buffer.as_mut_ptr(),
                &mut len,
                &mut is_pattern,
            )
        })?;
        let bytes = buffer
            .iter()
            .take(len)
            .map(|value| *value as u8)
            .collect::<Vec<_>>();
        let filter = String::from_utf8(bytes).map_err(|_| config_validation_error())?;
        Ok(Some((filter, is_pattern != 0)))
    }

    // -- Subscription event (XPUB) -----------------------------------------

    pub(crate) fn receive_subscription_event(
        &self,
        out: &mut SubscriptionEvent,
        flags: RecvFlags,
    ) -> Result<bool, RecvError> {
        let mut subscribed: i32 = 0;
        let mut topic_buf = [0i8; 256];
        let mut topic_len: usize = 256;
        let mut source_rid_ptr = ptr::null();

        let rc = unsafe {
            ffi::zlink_xpub_recv_part(
                self.handle,
                &mut source_rid_ptr,
                &mut subscribed,
                topic_buf.as_mut_ptr(),
                topic_buf.len(),
                &mut topic_len,
                flags.bits(),
            )
        };
        if rc == RecvResult::NoData as i32 {
            return Ok(false);
        }
        if rc != 0 {
            let errno = unsafe { ffi::zlink_errno() };
            if errno == libc::EAGAIN {
                return Ok(false);
            }
            return Err(RecvError::new(crate::error::RecvResult::Terminated, errno));
        }

        let topic = cstr_buf_to_smolstr(&topic_buf, topic_len);
        out.replace_from(SubscriptionEvent::new(
            routing_id_from_ptr(source_rid_ptr),
            subscribed != 0,
            topic,
        ));
        Ok(true)
    }

    // -- Callback installation ---------------------------------------------

    pub(crate) fn on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn() + Send + 'static,
    {
        let (cb, userdata) = CallbackBox::new(handler);
        if let Some(previous) = self.send_ready_cb.as_ref() {
            previous.set_closing(true);
        }
        let rc = unsafe {
            ffi::zlink_send_ready_handler(self.handle, send_ready_trampoline::<F>, userdata)
        };
        if rc != 0 {
            if let Some(previous) = self.send_ready_cb.as_ref() {
                previous.set_closing(false);
            }
            drop(cb);
            return check_handler_rc(rc);
        }
        let previous = self.send_ready_cb.replace(cb);
        if let Some(previous) = previous {
            crate::internal::release_callbacks(vec![previous]);
        }
        Ok(())
    }

    // -- Common typed options (per Option Policy) --------------------------

    pub(crate) fn set_send_high_water_mark(&self, value: u64) -> Result<(), ConfigError> {
        set_u64_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_SNDHWM, value)
    }

    pub(crate) fn send_high_water_mark(&self) -> Result<u64, ConfigError> {
        get_u64_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_SNDHWM)
    }

    pub(crate) fn set_receive_high_water_mark(&self, value: u64) -> Result<(), ConfigError> {
        set_u64_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_RCVHWM, value)
    }

    pub(crate) fn receive_high_water_mark(&self) -> Result<u64, ConfigError> {
        get_u64_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_RCVHWM)
    }

    pub(crate) fn set_linger(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_LINGER, d)
    }

    pub(crate) fn linger(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_LINGER)
    }

    pub(crate) fn set_send_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_SNDTIMEO, d)
    }

    pub(crate) fn send_timeout(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_SNDTIMEO)
    }

    pub(crate) fn set_receive_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_RCVTIMEO, d)
    }

    pub(crate) fn receive_timeout(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_RCVTIMEO)
    }

    pub(crate) fn set_reconnect_interval(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_RECONNECT_IVL, d)
    }

    pub(crate) fn reconnect_interval(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_RECONNECT_IVL)
    }

    pub(crate) fn set_reconnect_interval_max(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_RECONNECT_IVL_MAX,
            d,
        )
    }

    pub(crate) fn reconnect_interval_max(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_RECONNECT_IVL_MAX,
        )
    }

    pub(crate) fn set_submit_retry_mode(&self, value: i32) -> Result<(), ConfigError> {
        set_int_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_SUBMIT_RETRY_MODE,
            value,
        )
    }

    pub(crate) fn submit_retry_mode(&self) -> Result<i32, ConfigError> {
        get_int_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_SUBMIT_RETRY_MODE,
        )
    }

    pub(crate) fn set_submit_retry_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_SUBMIT_RETRY_TIMEOUT,
            d,
        )
    }

    pub(crate) fn submit_retry_timeout(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_SUBMIT_RETRY_TIMEOUT,
        )
    }

    pub(crate) fn set_submit_retry_attempts(&self, value: i32) -> Result<(), ConfigError> {
        set_int_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS,
            value,
        )
    }

    pub(crate) fn submit_retry_attempts(&self) -> Result<i32, ConfigError> {
        get_int_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS,
        )
    }

    pub(crate) fn set_max_message_size(&self, bytes: i64) -> Result<(), ConfigError> {
        let v = bytes;
        check_config_rc(unsafe {
            ffi::zlink_set_option(
                self.handle,
                ffi::zlink_option_t::ZLINK_OPT_MAXMSGSIZE,
                &v as *const i64 as *const c_void,
                std::mem::size_of::<i64>(),
            )
        })
    }

    pub(crate) fn max_message_size(&self) -> Result<i64, ConfigError> {
        get_i64_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_MAXMSGSIZE)
    }

    pub(crate) fn set_backlog(&self, value: i32) -> Result<(), ConfigError> {
        set_int_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_BACKLOG, value)
    }

    pub(crate) fn backlog(&self) -> Result<i32, ConfigError> {
        get_int_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_BACKLOG)
    }

    pub(crate) fn set_tcp_keepalive(&self, enabled: bool) -> Result<(), ConfigError> {
        set_bool_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_TCP_KEEPALIVE,
            enabled,
        )
    }

    pub(crate) fn tcp_keepalive(&self) -> Result<bool, ConfigError> {
        get_bool_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_TCP_KEEPALIVE)
    }

    pub(crate) fn set_tcp_no_delay(&self, enabled: bool) -> Result<(), ConfigError> {
        set_bool_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_TCP_NODELAY,
            enabled,
        )
    }

    pub(crate) fn tcp_no_delay(&self) -> Result<bool, ConfigError> {
        get_bool_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_TCP_NODELAY)
    }

    pub(crate) fn set_ipv6(&self, enabled: bool) -> Result<(), ConfigError> {
        set_bool_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_IPV6, enabled)
    }

    pub(crate) fn ipv6(&self) -> Result<bool, ConfigError> {
        get_bool_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_IPV6)
    }

    pub(crate) fn set_immediate(&self, enabled: bool) -> Result<(), ConfigError> {
        set_bool_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_IMMEDIATE,
            enabled,
        )
    }

    pub(crate) fn immediate(&self) -> Result<bool, ConfigError> {
        get_bool_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_IMMEDIATE)
    }

    pub(crate) fn set_connect_timeout(&self, d: Duration) -> Result<(), ConfigError> {
        set_duration_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_CONNECT_TIMEOUT,
            d,
        )
    }

    pub(crate) fn connect_timeout(&self) -> Result<Duration, ConfigError> {
        get_duration_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_CONNECT_TIMEOUT)
    }

    pub(crate) fn set_rid_duplicate_policy(&self, value: i32) -> Result<(), ConfigError> {
        set_int_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_RID_DUPLICATE_POLICY,
            value,
        )
    }

    pub(crate) fn rid_duplicate_policy(&self) -> Result<i32, ConfigError> {
        get_int_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_RID_DUPLICATE_POLICY,
        )
    }

    pub(crate) fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_set_routing_id(self.handle, id.data().as_ptr() as *const c_void, id.len())
        })
    }

    pub(crate) fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        let mut raw = MaybeUninit::<ffi::zlink_routing_id_t>::uninit();
        check_config_rc(unsafe { ffi::zlink_get_routing_id(self.handle, raw.as_mut_ptr()) })?;
        Ok(RoutingId::from_raw(unsafe { raw.assume_init() }))
    }

    pub(crate) fn last_endpoint(&self) -> Result<String, ConfigError> {
        let mut buf = [0u8; 256];
        let mut len = buf.len();
        check_config_rc(unsafe {
            ffi::zlink_get_option(
                self.handle,
                ffi::zlink_option_t::ZLINK_OPT_LAST_ENDPOINT,
                buf.as_mut_ptr() as *mut c_void,
                &mut len,
            )
        })?;
        let s = unsafe { CStr::from_ptr(buf.as_ptr() as *const i8) };
        Ok(s.to_string_lossy().into_owned())
    }

    pub(crate) fn set_tls_server(
        &self,
        cert: &str,
        key: &str,
        require_client_cert: bool,
    ) -> Result<(), ConfigError> {
        let c_cert = CString::new(cert).map_err(|_| config_validation_error())?;
        let c_key = CString::new(key).map_err(|_| config_validation_error())?;
        match check_config_rc(unsafe {
            ffi::zlink_set_tls_server(
                self.handle,
                c_cert.as_ptr(),
                c_key.as_ptr(),
                if require_client_cert { 1 } else { 0 },
            )
        }) {
            Ok(()) => Ok(()),
            Err(err) if err.code() == crate::error::ConfigResult::InvalidHandle => {
                self.set_tls_cert(cert)?;
                self.set_tls_key(key)?;
                set_bool_opt(
                    self.handle,
                    ffi::zlink_option_t::ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT,
                    require_client_cert,
                )
            }
            Err(err) => Err(err),
        }
    }

    pub(crate) fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
        set_string_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_TLS_CERT, cert)
    }

    pub(crate) fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
        set_string_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_TLS_KEY, key)
    }

    pub(crate) fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
        set_string_opt(self.handle, ffi::zlink_option_t::ZLINK_OPT_TLS_CA, ca_cert)
    }

    pub(crate) fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
        set_string_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_TLS_HOSTNAME,
            hostname,
        )
    }

    pub(crate) fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
        set_bool_opt(
            self.handle,
            ffi::zlink_option_t::ZLINK_OPT_TLS_TRUST_SYSTEM,
            trust_system,
        )
    }

    pub(crate) fn set_tls_client(
        &self,
        ca_cert: &str,
        hostname: &str,
        trust_system: bool,
    ) -> Result<(), ConfigError> {
        let c_ca = CString::new(ca_cert).map_err(|_| config_validation_error())?;
        let c_host = CString::new(hostname).map_err(|_| config_validation_error())?;
        match check_config_rc(unsafe {
            ffi::zlink_set_tls_client(
                self.handle,
                c_ca.as_ptr(),
                c_host.as_ptr(),
                if trust_system { 1 } else { 0 },
            )
        }) {
            Ok(()) => Ok(()),
            Err(err) if err.code() == crate::error::ConfigResult::InvalidHandle => {
                self.set_tls_ca(ca_cert)?;
                self.set_tls_hostname(hostname)?;
                self.set_tls_trust_system(trust_system)
            }
            Err(err) => Err(err),
        }
    }

    pub(crate) fn close(&mut self) -> Result<(), CloseError> {
        if self.handle.is_null() {
            return Ok(());
        }
        for callback in self.send_ready_cb.iter().chain(self.packet_cb.iter()) {
            callback.set_closing(true);
        }
        if let Err(error) = check_close_rc(unsafe { ffi::zlink_close(self.handle) }) {
            for callback in self.send_ready_cb.iter().chain(self.packet_cb.iter()) {
                callback.set_closing(false);
            }
            return Err(error);
        }
        self.handle = ptr::null_mut();
        let callbacks = [self.send_ready_cb.take(), self.packet_cb.take()]
            .into_iter()
            .flatten()
            .collect();
        crate::internal::release_callbacks(callbacks);
        Ok(())
    }

    // -- PUB-option helpers ------------------------------------------------
    //
    // These were previously duplicated as free functions in pub_socket.rs and
    // xpub.rs plus pub(crate) methods on both PubSocket and XPubSocket. Hosting
    // them on SocketInner lets PubSocketOptions hold a `&SocketInner` directly
    // and call them without the trait + impl trampoline chain.

    pub(crate) fn set_pub_bool_opt(
        &self,
        opt: ffi::zlink_pub_option_t,
        value: bool,
    ) -> Result<(), ConfigError> {
        let v: i32 = if value { 1 } else { 0 };
        check_config_rc(unsafe {
            ffi::zlink_set_pub_option(
                self.handle,
                opt,
                &v as *const i32 as *const c_void,
                std::mem::size_of::<i32>(),
            )
        })
    }

    pub(crate) fn get_pub_int_opt(&self, opt: ffi::zlink_pub_option_t) -> Result<i32, ConfigError> {
        let mut value: i32 = 0;
        let mut len = std::mem::size_of::<i32>();
        check_config_rc(unsafe {
            ffi::zlink_get_pub_option(
                self.handle,
                opt,
                &mut value as *mut i32 as *mut c_void,
                &mut len,
            )
        })?;
        Ok(value)
    }

    pub(crate) fn get_pub_bool_opt(
        &self,
        opt: ffi::zlink_pub_option_t,
    ) -> Result<bool, ConfigError> {
        Ok(self.get_pub_int_opt(opt)? != 0)
    }

    pub(crate) fn set_pub_bytes_opt(
        &self,
        opt: ffi::zlink_pub_option_t,
        value: &[u8],
    ) -> Result<(), ConfigError> {
        let ptr = if value.is_empty() {
            std::ptr::null()
        } else {
            value.as_ptr() as *const c_void
        };
        check_config_rc(unsafe { ffi::zlink_set_pub_option(self.handle, opt, ptr, value.len()) })
    }

    pub(crate) fn get_pub_message_opt(
        &self,
        opt: ffi::zlink_pub_option_t,
    ) -> Result<crate::message::Message, ConfigError> {
        // Two-step query: first probe the length with a null buffer, then
        // allocate exactly what's needed. Previously this always allocated a
        // 64 KiB scratch buffer up front; for the welcome-message case the
        // payload is typically a few hundred bytes at most.
        let mut len: usize = 0;
        check_config_rc(unsafe {
            ffi::zlink_get_pub_option(self.handle, opt, std::ptr::null_mut(), &mut len)
        })?;
        if len == 0 {
            return crate::message::Message::try_from([]);
        }
        let mut buf = vec![0u8; len];
        let mut filled = len;
        check_config_rc(unsafe {
            ffi::zlink_get_pub_option(
                self.handle,
                opt,
                buf.as_mut_ptr() as *mut c_void,
                &mut filled,
            )
        })?;
        crate::message::Message::try_from(&buf[..filled])
    }

    // -- SUB-option helper -------------------------------------------------

    pub(crate) fn get_sub_int_opt(&self, opt: ffi::zlink_sub_option_t) -> Result<i32, ConfigError> {
        let mut value: i32 = 0;
        let mut len = std::mem::size_of::<i32>();
        check_config_rc(unsafe {
            ffi::zlink_get_sub_option(
                self.handle,
                opt,
                &mut value as *mut i32 as *mut c_void,
                &mut len,
            )
        })?;
        Ok(value)
    }

    // -- ROUTER-option helpers --------------------------------------------
    //
    // Same shape as the PUB/SUB helpers above: RouterSocketOptions holds a
    // `&SocketInner` and dispatches here directly, eliminating the previous
    // wrapper -> trait -> per-type forward indirection.

    pub(crate) fn set_router_bool_opt(
        &self,
        opt: ffi::zlink_router_option_t,
        value: bool,
    ) -> Result<(), ConfigError> {
        let v: i32 = if value { 1 } else { 0 };
        check_config_rc(unsafe {
            ffi::zlink_set_router_option(
                self.handle,
                opt,
                &v as *const i32 as *const c_void,
                std::mem::size_of::<i32>(),
            )
        })
    }

    pub(crate) fn set_router_i32_opt(
        &self,
        opt: ffi::zlink_router_option_t,
        value: i32,
    ) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_set_router_option(
                self.handle,
                opt,
                &value as *const i32 as *const c_void,
                std::mem::size_of::<i32>(),
            )
        })
    }

    pub(crate) fn set_router_u32_opt(
        &self,
        opt: ffi::zlink_router_option_t,
        value: u32,
    ) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_set_router_option(
                self.handle,
                opt,
                &value as *const u32 as *const c_void,
                std::mem::size_of::<u32>(),
            )
        })
    }

    pub(crate) fn set_router_bytes_opt(
        &self,
        opt: ffi::zlink_router_option_t,
        value: &[u8],
    ) -> Result<(), ConfigError> {
        let ptr = if value.is_empty() {
            std::ptr::null()
        } else {
            value.as_ptr() as *const c_void
        };
        check_config_rc(unsafe { ffi::zlink_set_router_option(self.handle, opt, ptr, value.len()) })
    }

    pub(crate) fn get_router_i32_opt(
        &self,
        opt: ffi::zlink_router_option_t,
    ) -> Result<i32, ConfigError> {
        let mut value: i32 = 0;
        let mut len = std::mem::size_of::<i32>();
        check_config_rc(unsafe {
            ffi::zlink_get_router_option(
                self.handle,
                opt,
                &mut value as *mut i32 as *mut c_void,
                &mut len,
            )
        })?;
        Ok(value)
    }

    pub(crate) fn get_router_u32_opt(
        &self,
        opt: ffi::zlink_router_option_t,
    ) -> Result<u32, ConfigError> {
        let mut value: u32 = 0;
        let mut len = std::mem::size_of::<u32>();
        check_config_rc(unsafe {
            ffi::zlink_get_router_option(
                self.handle,
                opt,
                &mut value as *mut u32 as *mut c_void,
                &mut len,
            )
        })?;
        Ok(value)
    }

    // -- DEALER-option helpers --------------------------------------------

    pub(crate) fn get_dealer_u32_opt(
        &self,
        opt: ffi::zlink_dealer_option_t,
    ) -> Result<u32, ConfigError> {
        let mut value: u32 = 0;
        let mut len = std::mem::size_of::<u32>();
        check_config_rc(unsafe {
            ffi::zlink_get_dealer_option(
                self.handle,
                opt,
                &mut value as *mut u32 as *mut c_void,
                &mut len,
            )
        })?;
        Ok(value)
    }

    pub(crate) fn set_dealer_bool_opt(
        &self,
        opt: ffi::zlink_dealer_option_t,
        value: bool,
    ) -> Result<(), ConfigError> {
        let v: i32 = if value { 1 } else { 0 };
        check_config_rc(unsafe {
            ffi::zlink_set_dealer_option(
                self.handle,
                opt,
                &v as *const i32 as *const c_void,
                std::mem::size_of::<i32>(),
            )
        })
    }

    pub(crate) fn set_dealer_u32_opt(
        &self,
        opt: ffi::zlink_dealer_option_t,
        value: u32,
    ) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_set_dealer_option(
                self.handle,
                opt,
                &value as *const u32 as *const c_void,
                std::mem::size_of::<u32>(),
            )
        })
    }

    pub(crate) fn set_dealer_i32_opt(
        &self,
        opt: ffi::zlink_dealer_option_t,
        value: i32,
    ) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_set_dealer_option(
                self.handle,
                opt,
                &value as *const i32 as *const c_void,
                std::mem::size_of::<i32>(),
            )
        })
    }

    // -- STREAM-option helpers --------------------------------------------

    pub(crate) fn set_stream_bool_opt(
        &self,
        opt: ffi::zlink_stream_option_t,
        value: bool,
    ) -> Result<(), ConfigError> {
        let v: i32 = if value { 1 } else { 0 };
        check_config_rc(unsafe {
            ffi::zlink_set_stream_option(
                self.handle,
                opt,
                &v as *const i32 as *const c_void,
                std::mem::size_of::<i32>(),
            )
        })
    }

    pub(crate) fn get_stream_bool_opt(
        &self,
        opt: ffi::zlink_stream_option_t,
    ) -> Result<bool, ConfigError> {
        let mut value: i32 = 0;
        let mut len = std::mem::size_of::<i32>();
        check_config_rc(unsafe {
            ffi::zlink_get_stream_option(
                self.handle,
                opt,
                &mut value as *mut i32 as *mut c_void,
                &mut len,
            )
        })?;
        Ok(value != 0)
    }
}

impl Drop for crate::internal::SocketStorage {
    fn drop(&mut self) {
        if self.handle.is_null() {
            return;
        }
        for callback in self.send_ready_cb.iter().chain(self.packet_cb.iter()) {
            callback.set_closing(true);
        }
        let callbacks = [self.send_ready_cb.take(), self.packet_cb.take()]
            .into_iter()
            .flatten()
            .collect();
        let rc = unsafe { ffi::zlink_close(self.handle) };
        if rc == 0 {
            crate::internal::release_callbacks(callbacks);
        } else {
            crate::internal::defer_native_close(
                crate::internal::DeferredCloseKind::Socket,
                self.handle,
                callbacks,
            );
        }
    }
}

pub(crate) unsafe extern "C" fn send_ready_trampoline<F: Fn() + Send + 'static>(
    _subject: *mut c_void,
    userdata: *mut c_void,
) {
    let _ = unsafe { CallbackBox::invoke::<F, _>(userdata, |handler| handler()) };
}
