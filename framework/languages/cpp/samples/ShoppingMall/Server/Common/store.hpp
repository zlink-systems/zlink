/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/Contracts/messages.hpp"
#include "../Configuration/sample_topology.hpp"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <istream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::samples::shoppingmall
{

class redis_state_store_t
{
  public:
    explicit redis_state_store_t (sample_topology_t topology) : _topology (std::move (topology))
    {
        if (_topology.redis_endpoint.empty ()) {
            throw std::runtime_error ("SHOPPINGMALL_REDIS_ENDPOINT is required");
        }
    }

    template <typename TFunc> decltype (auto) update (TFunc &&func) const
    {
        redis_lock_t lock{*this};
        auto state = load ();
        if constexpr (std::is_void_v<std::invoke_result_t<TFunc, nlohmann::json &>>) {
            func (state);
            save (state);
            return;
        } else {
            decltype (auto) result = func (state);
            save (state);
            return result;
        }
    }

    template <typename TFunc> decltype (auto) read (TFunc &&func) const
    {
        redis_lock_t lock{*this};
        auto state = load ();
        return func (state);
    }

    /* 공통 sample spec §11: runner가 띄운 CommerceApi가 self-check 시드를 멱등하게 넣는다.
     * 재고 실패는 AvailableQuantity 부족 장바구니가, 결제 실패는 ShouldAuthorize=false 결제수단이
     * 만든다 — 금액 범위나 문자열 비교로 흉내내지 않는다. */
    void seed_defaults () const
    {
        update ([] (nlohmann::json &state) {
            if (!state.empty ()) return;
            state = nlohmann::json::object ();
            state["nextOrderSequence"] = 0;
            state["idempotency"] = nlohmann::json::object ();
            state["events"] = nlohmann::json::object ();
            state["readModels"] = nlohmann::json::object ();
            state["orderCommands"] = nlohmann::json::object ();
            state["reservations"] = nlohmann::json::object ();
            state["payments"] = nlohmann::json::object ();
            state["releasedReservations"] = nlohmann::json::object ();
            state["paymentAttempts"] = nlohmann::json::object ();
            state["testHooks"] = nlohmann::json::object ();

            state["carts"] = nlohmann::json::object ();
            state["carts"]["cart-success"] =
              cart_seed_t{"cart-success", {{"sku-ok", 1}}, decimal_t ("120.00"), "USD"};
            state["carts"]["cart-inventory-fail"] =
              cart_seed_t{"cart-inventory-fail", {{"sku-rare", 1}}, decimal_t ("120.00"),
                          "USD"};

            state["inventory"] = nlohmann::json::object ();
            state["inventory"]["sku-ok"] = inventory_seed_t{"sku-ok", 100};
            state["inventory"]["sku-rare"] = inventory_seed_t{"sku-rare", 0};

            state["paymentMethods"] = nlohmann::json::object ();
            state["paymentMethods"]["pm-ok"] = payment_method_seed_t{"pm-ok", true, ""};
            state["paymentMethods"]["pm-decline"] =
              payment_method_seed_t{"pm-decline", false, "payment declined"};
        });
    }

  private:
    class redis_lock_t
    {
      public:
        explicit redis_lock_t (const redis_state_store_t &store) : _store (store), _token (make_token ())
        {
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
            while (std::chrono::steady_clock::now () < deadline) {
                if (_store.execute_string (
                      {"SET", _store.lock_key (), _token, "NX", "PX", "30000"}) == "OK") {
                    _locked = true;
                    return;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (25));
            }
            throw std::runtime_error ("failed to lock ShoppingMall Redis state");
        }

        redis_lock_t (const redis_lock_t &) = delete;
        redis_lock_t &operator= (const redis_lock_t &) = delete;

        ~redis_lock_t ()
        {
            if (_locked) {
                try {
                    (void) _store.execute_integer (
                      {"EVAL",
                       "if redis.call('GET', KEYS[1]) == ARGV[1] then return "
                       "redis.call('DEL', KEYS[1]) else return 0 end",
                       "1",
                       _store.lock_key (),
                       _token});
                }
                catch (...) {
                }
            }
        }

      private:
        static std::string make_token ()
        {
            static thread_local std::mt19937_64 rng{std::random_device{} ()};
            return std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ())
                   + ":" + std::to_string (rng ());
        }

        const redis_state_store_t &_store;
        std::string _token;
        bool _locked{};
    };

    nlohmann::json load () const
    {
        const auto stored = execute_string ({"GET", state_key ()});
        if (stored.empty ()) {
            return nlohmann::json::object ();
        }
        std::istringstream input (stored);
        nlohmann::json state;
        input >> state;
        return state;
    }

    void save (const nlohmann::json &state) const
    {
        if (execute_string ({"SET", state_key (), state.dump (2)}) != "OK") {
            throw std::runtime_error ("Redis did not store ShoppingMall state");
        }
    }

    std::string state_key () const
    {
        return _topology.redis_key_prefix + "shoppingmall:commerce-state";
    }

    std::string lock_key () const
    {
        return _topology.redis_key_prefix + "shoppingmall:commerce-state:lock";
    }

    static std::pair<std::string, std::string> split_endpoint (std::string endpoint)
    {
        if (endpoint.rfind ("tcp://", 0) == 0) {
            endpoint = endpoint.substr (6);
        } else if (endpoint.rfind ("redis://", 0) == 0) {
            endpoint = endpoint.substr (8);
        }
        const auto separator = endpoint.rfind (':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == endpoint.size ()) {
            throw std::runtime_error ("Redis endpoint must use host:port");
        }
        return {endpoint.substr (0, separator), endpoint.substr (separator + 1)};
    }

    std::string execute_string (const std::vector<std::string> &args) const
    {
        auto reply = execute (args);
        if (reply.type == '+' || reply.type == '$') {
            return reply.value;
        }
        if (reply.type == ':') {
            return reply.value;
        }
        throw std::runtime_error ("Redis command returned an unsupported response");
    }

    long long execute_integer (const std::vector<std::string> &args) const
    {
        auto reply = execute (args);
        if (reply.type != ':') {
            throw std::runtime_error ("Redis command did not return an integer");
        }
        return std::stoll (reply.value);
    }

    struct redis_reply_t
    {
        char type{};
        std::string value;
    };

    redis_reply_t execute (const std::vector<std::string> &args) const
    {
        const auto [host, port] = split_endpoint (_topology.redis_endpoint);
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver (io);
        boost::asio::ip::tcp::socket socket (io);
        boost::asio::connect (socket, resolver.resolve (host, port));
        boost::asio::write (socket, boost::asio::buffer (encode_command (args)));
        boost::asio::streambuf buffer;
        return read_response (socket, buffer);
    }

    static std::string encode_command (const std::vector<std::string> &args)
    {
        std::string command = "*" + std::to_string (args.size ()) + "\r\n";
        for (const auto &arg : args) {
            command += "$" + std::to_string (arg.size ()) + "\r\n";
            command += arg;
            command += "\r\n";
        }
        return command;
    }

    static std::string read_line (boost::asio::ip::tcp::socket &socket,
                                  boost::asio::streambuf &buffer)
    {
        boost::asio::read_until (socket, buffer, "\r\n");
        std::istream input (&buffer);
        std::string line;
        std::getline (input, line);
        if (!line.empty () && line.back () == '\r') {
            line.pop_back ();
        }
        return line;
    }

    static redis_reply_t read_response (boost::asio::ip::tcp::socket &socket,
                                        boost::asio::streambuf &buffer)
    {
        const auto header = read_line (socket, buffer);
        if (header.empty ()) {
            throw std::runtime_error ("Redis returned an empty response");
        }
        if (header[0] == '+' || header[0] == ':') {
            return {header[0], header.substr (1)};
        }
        if (header[0] == '$') {
            const auto length = std::stoll (header.substr (1));
            if (length < 0) {
                return {'$', {}};
            }
            while (buffer.size () < static_cast<std::size_t> (length + 2)) {
                boost::asio::read (socket, buffer, boost::asio::transfer_at_least (1));
            }
            std::string value (static_cast<std::size_t> (length), '\0');
            std::istream input (&buffer);
            input.read (value.data (), length);
            char cr = 0;
            char lf = 0;
            input.get (cr);
            input.get (lf);
            return {'$', value};
        }
        if (header[0] == '-') {
            throw std::runtime_error ("Redis error: " + header.substr (1));
        }
        throw std::runtime_error ("Unsupported Redis response: " + header);
    }

    sample_topology_t _topology;
};

inline std::int64_t now_unix_ms ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (
             std::chrono::system_clock::now ().time_since_epoch ())
      .count ();
}

inline order_state_t state_from_json (const nlohmann::json &json)
{
    return json.get<order_state_t> ();
}

inline std::vector<std::string> event_types_for (const nlohmann::json &state,
                                                 const std::string &order_id)
{
    std::vector<std::string> result;
    if (!state["events"].contains (order_id)) return result;
    for (const auto &event : state["events"][order_id]) {
        result.push_back (event.value ("eventType", ""));
    }
    return result;
}

inline bool has_sequence (const nlohmann::json &state,
                          const std::string &order_id,
                          const std::vector<std::string> &expected)
{
    return event_types_for (state, order_id) == expected;
}

inline bool has_prefix (const nlohmann::json &state,
                        const std::string &order_id,
                        const std::vector<std::string> &expected)
{
    const auto actual = event_types_for (state, order_id);
    return actual.size () >= expected.size ()
           && std::equal (expected.begin (), expected.end (), actual.begin ());
}

} // namespace zlink::samples::shoppingmall
