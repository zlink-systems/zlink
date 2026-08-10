/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "socket_contracts.hpp"

namespace zlink
{

/// @brief Base for publisher sockets (PUB and XPUB).
class publisher_socket_t : public socket_t
{
  protected:
    publisher_socket_t (context_t &ctx_, socket_type type_) : socket_t (ctx_, type_) {}
};

} // namespace zlink

namespace zlink
{

/// @brief Base for subscriber sockets (SUB and XSUB).
class subscriber_socket_t : public socket_t
{
  public:
    [[nodiscard]] int set_subscription (const std::string &filter_)
    {
        return socket_t::set_subscription (filter_);
    }

    [[nodiscard]] int unset_subscription (const std::string &filter_)
    {
        return socket_t::unset_subscription (filter_);
    }

    [[nodiscard]] int
    subscription_at (size_t index_, std::string &filter_, bool *is_pattern_ = nullptr)
    {
        return socket_t::subscription_at (index_, filter_, is_pattern_);
    }

    [[nodiscard]] topic_message_t subscribe ()
    {
        topic_message_t message;
        const int rc = socket_t::subscribe (message);
        throw_on_error (rc);
        return message;
    }

  protected:
    subscriber_socket_t (context_t &ctx_, socket_type type_) : socket_t (ctx_, type_) {}
};

} // namespace zlink

namespace zlink
{

/// @brief A topic-filtered publisher that drops messages with no matching subscriber.
class pub_socket_t : public publisher_socket_t
{
  public:
    explicit pub_socket_t (context_t &ctx_);

    send_operation_t publish (const std::string &topic_id_);

    void set_send_ready_handler (std::function<void ()> handler_)
    {
        socket_t::set_send_ready_handler (std::move (handler_));
    }

    pub_socket_options_t options () { return pub_socket_options_t (*this); }

  private:
    using publisher_socket_t::set_send_ready_handler;
};

} // namespace zlink

namespace zlink
{

/// @brief A receive-only subscriber filtered by its subscriptions.
class sub_socket_t : public subscriber_socket_t
{
  public:
    explicit sub_socket_t (context_t &ctx_);

    void set_subscription (const std::string &filter_);

    void unset_subscription (const std::string &filter_);

    void subscription_at (size_t index_, std::string &filter_out_, bool *is_pattern_out_ = nullptr);

    subscription_filter_t subscription_at (size_t index_)
    {
        subscription_filter_t filter;
        subscription_at (index_, filter.filter, &filter.is_pattern);
        return filter;
    }

    int subscribe (topic_message_t &out_, recv_flags_t flags_ = recv_flags_t::none)
    {
        return socket_t::subscribe (out_, flags_);
    }

    int subscribe_part (std::optional<routing_id_t> &source_rid_out_,
                        std::string &topic_out_,
                        message_t &part_out_,
                        bool &has_more_out_,
                        recv_flags_t flags_ = recv_flags_t::none)
    {
        return socket_t::subscribe_part (source_rid_out_, topic_out_, part_out_, has_more_out_,
                                         flags_);
    }

    sub_socket_options_t options () { return sub_socket_options_t (*this); }

  private:
    using subscriber_socket_t::set_subscription;
    using subscriber_socket_t::subscription_at;
    using subscriber_socket_t::unset_subscription;
};

} // namespace zlink

namespace zlink
{

/// @brief A publisher that also surfaces subscriber subscription events.
class xpub_socket_t : public publisher_socket_t
{
  public:
    explicit xpub_socket_t (context_t &ctx_);

    send_operation_t publish (const std::string &topic_id_);

    void set_send_ready_handler (std::function<void ()> handler_)
    {
        socket_t::set_send_ready_handler (std::move (handler_));
    }

    int receive_subscription_event (subscription_event_t &out_,
                                    recv_flags_t flags_ = recv_flags_t::none);

    pub_socket_options_t options () { return pub_socket_options_t (*this); }

  private:
    using publisher_socket_t::set_send_ready_handler;
};

} // namespace zlink

namespace zlink
{

/// @brief A subscriber whose subscriptions are carried as messages.
class xsub_socket_t : public subscriber_socket_t
{
  public:
    explicit xsub_socket_t (context_t &ctx_);

    void set_subscription (const std::string &filter_);

    void unset_subscription (const std::string &filter_);

    void subscription_at (size_t index_, std::string &filter_out_, bool *is_pattern_out_ = nullptr);

    subscription_filter_t subscription_at (size_t index_)
    {
        subscription_filter_t filter;
        subscription_at (index_, filter.filter, &filter.is_pattern);
        return filter;
    }

    int subscribe (topic_message_t &out_, recv_flags_t flags_ = recv_flags_t::none)
    {
        return socket_t::subscribe (out_, flags_);
    }

    int subscribe_part (std::optional<routing_id_t> &source_rid_out_,
                        std::string &topic_out_,
                        message_t &part_out_,
                        bool &has_more_out_,
                        recv_flags_t flags_ = recv_flags_t::none)
    {
        return socket_t::subscribe_part (source_rid_out_, topic_out_, part_out_, has_more_out_,
                                         flags_);
    }

    sub_socket_options_t options () { return sub_socket_options_t (*this); }

  private:
    using subscriber_socket_t::set_subscription;
    using subscriber_socket_t::subscription_at;
    using subscriber_socket_t::unset_subscription;
};

} // namespace zlink
