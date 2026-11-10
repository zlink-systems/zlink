/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/locations/etcd.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::etcd;
using json = nlohmann::json;

constexpr std::string_view alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

template <typename T> T integer (const json &input)
{
    if (!input.is_number_integer ())
        throw std::runtime_error ("expected JSON integer");
    const auto convert = [] (auto value) -> T {
        if (!std::in_range<T> (value))
            throw std::runtime_error ("JSON integer exceeds its native range");
        return static_cast<T> (value);
    };
    return input.is_number_unsigned () ? convert (input.get<std::uint64_t> ())
                                       : convert (input.get<std::int64_t> ());
}

std::string base64 (const std::vector<std::byte> &bytes)
{
    std::string output;
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const auto byte : bytes) {
        accumulator = (accumulator << 8) | std::to_integer<unsigned> (byte);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output += alphabet[(accumulator >> bits) & 63];
        }
    }
    if (bits)
        output += alphabet[(accumulator << (6 - bits)) & 63];
    while (output.size () % 4)
        output += '=';
    return output;
}

std::vector<std::byte> bytes (const json &input)
{
    const auto text = input.get<std::string> ();
    std::vector<std::byte> output;
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const auto character : text) {
        if (character == '=')
            break;
        const auto digit = alphabet.find (character);
        if (digit == std::string_view::npos)
            throw std::runtime_error ("expected canonical base64");
        accumulator = (accumulator << 6) | static_cast<unsigned> (digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back (static_cast<std::byte> ((accumulator >> bits) & 255));
        }
    }
    if (base64 (output) != text)
        throw std::runtime_error ("expected canonical base64");
    return output;
}

std::chrono::milliseconds decimal (const json &input)
{
    const auto text = input.get<std::string> ();
    const auto digits = std::string_view (text).substr (!text.empty () && text[0] == '-' ? 1 : 0);
    std::int64_t value = 0;
    const auto parsed = std::from_chars (text.data (), text.data () + text.size (), value);
    if (digits.empty () || (digits.size () > 1 && digits[0] == '0') || parsed.ec != std::errc ()
        || parsed.ptr != text.data () + text.size ())
        throw std::runtime_error ("expected integer decimal milliseconds");
    return std::chrono::milliseconds (value);
}

std::string milliseconds (std::chrono::system_clock::time_point time)
{
    return std::to_string (
      std::chrono::duration_cast<std::chrono::milliseconds> (time.time_since_epoch ()).count ());
}

json value (const store_value_t &input)
{
    return {
      {"bytesBase64", base64 (input.bytes)},
      {"version", input.version.value},
      {"expiresAtMs", input.expires_at ? json (milliseconds (*input.expires_at)) : json (nullptr)},
      {"storeNowMs", milliseconds (input.store_now)}};
}

json result (const store_read_result_t &input)
{
    if (const auto *found = std::get_if<store_found_t> (&input))
        return {{"kind", "found"}, {"value", value (found->value)}};
    return {{"kind", "missing"},
            {"storeNowMs", milliseconds (std::get<store_missing_t> (input).store_now)}};
}

json result (const store_write_result_t &input)
{
    if (const auto *applied = std::get_if<store_write_applied_t> (&input)) {
        auto versions = json::array ();
        for (const auto &item : applied->put_versions)
            versions.push_back ({{"key", item.key.value}, {"version", item.version.value}});
        return {{"kind", "applied"},
                {"storeNowMs", milliseconds (applied->store_now)},
                {"putVersions", std::move (versions)}};
    }
    return {{"kind", "conflict"},
            {"storeNowMs", milliseconds (std::get<store_write_conflict_t> (input).store_now)}};
}

json result (const store_scan_result_t &input)
{
    if (const auto *page = std::get_if<store_scan_page_t> (&input)) {
        auto items = json::array ();
        for (const auto &item : page->items)
            items.push_back ({{"key", item.key.value}, {"value", value (item.value)}});
        return {
          {"kind", "page"},
          {"value",
           {{"storeNowMs", milliseconds (page->store_now)},
            {"items", std::move (items)},
            {"nextCursor", page->next_cursor ? json (page->next_cursor->value) : json (nullptr)}}}};
    }
    return {{"kind", "expired"}};
}

etcd_location_options_t options (const json &input)
{
    etcd_location_options_t output;
    output.endpoints = input.at ("endpoints").get<std::vector<std::string>> ();
    output.key_namespace = input.at ("namespace").get<std::string> ();
    if (input.contains ("operationTimeoutMs"))
        output.operation_timeout =
          std::chrono::milliseconds (integer<std::int64_t> (input.at ("operationTimeoutMs")));
    if (input.contains ("credentials")) {
        const auto &credentials = input.at ("credentials");
        output.credentials = etcd_credentials_t{credentials.at ("user").get<std::string> (),
                                                credentials.at ("password").get<std::string> ()};
    }
    if (input.contains ("tls")) {
        const auto &tls = input.at ("tls");
        output.tls = etcd_tls_options_t{tls.value ("trustedCaPem", std::string ()), {}};
        if (tls.contains ("clientIdentity")) {
            const auto &identity = tls.at ("clientIdentity");
            output.tls->client_identity =
              etcd_client_identity_t{identity.at ("certificatePem").get<std::string> (),
                                     identity.at ("keyPem").get<std::string> ()};
        }
    }
    return output;
}

store_write_request_t write_request (const json &input)
{
    store_write_request_t output;
    for (const auto &condition : input.at ("conditions")) {
        const store_key_t key{condition.at ("key").get<std::string> ()};
        const auto kind = condition.at ("kind").get<std::string> ();
        if (kind == "missing")
            output.conditions.emplace_back (store_missing_condition_t{key});
        else if (kind == "version")
            output.conditions.emplace_back (
              store_version_condition_t{key, {condition.at ("version").get<std::string> ()}});
        else if (kind == "value")
            output.conditions.emplace_back (
              store_value_condition_t{key, bytes (condition.at ("bytesBase64"))});
        else
            throw std::runtime_error ("unknown condition kind");
    }
    for (const auto &mutation : input.at ("mutations")) {
        const store_key_t key{mutation.at ("key").get<std::string> ()};
        const auto kind = mutation.at ("kind").get<std::string> ();
        if (kind == "delete")
            output.mutations.emplace_back (store_delete_t{key});
        else if (kind == "put") {
            store_put_t put{key, bytes (mutation.at ("bytesBase64")), {}};
            if (mutation.contains ("retentionMs"))
                put.retention = decimal (mutation.at ("retentionMs"));
            output.mutations.emplace_back (std::move (put));
        } else
            throw std::runtime_error ("unknown mutation kind");
    }
    return output;
}

store_scan_request_t scan_request (const json &input)
{
    store_scan_request_t output;
    output.prefix = input.at ("prefix").get<std::string> ();
    output.limit = integer<std::uint32_t> (input.at ("limit"));
    if (input.contains ("cursor") && !input.at ("cursor").is_null ())
        output.cursor = store_scan_cursor_t{input.at ("cursor").get<std::string> ()};
    return output;
}

class adapter_t
{
    struct operation_t
    {
        bool started = false;
        bool cancelled = false;
    };
    struct store_t
    {
        std::unique_ptr<etcd_location_store_t> provider;
        std::size_t active = 0;
    };

  public:
    void accept (const std::string &line)
    {
        json message;
        json response_id = nullptr;
        std::string response_op;
        bool provider = false;
        try {
            message = json::parse (line);
            if (message.at ("v") != 1 || !message.at ("id").is_string ())
                throw std::runtime_error ("invalid protocol version or request id");
            const auto id = message.at ("id").get<std::string> ();
            response_id = id;
            if (!_seen.insert (id).second)
                throw std::runtime_error ("duplicate request id");
            const auto op = message.at ("op").get<std::string> ();
            response_op = op;
            if (op == "cancel") {
                bool active;
                {
                    std::lock_guard lock (_mutex);
                    const auto found = _operations.find (message.at ("target").get<std::string> ());
                    active = found != _operations.end ();
                    if (active && !found->second->started)
                        found->second->cancelled = true;
                }
                reply (id, {{"ok", true}, {"result", {{"kind", "cancelled"}, {"active", active}}}});
                return;
            }
            const auto name = message.at ("store").get<std::string> ();
            if (op == "create") {
                if (_stores.contains (name))
                    throw std::runtime_error ("duplicate store");
                auto store = std::make_shared<store_t> ();
                auto native_options = options (message.at ("options"));
                provider = true;
                store->provider =
                  std::make_unique<etcd_location_store_t> (std::move (native_options));
                provider = false;
                _stores.emplace (name, std::move (store));
                reply (id, {{"ok", true}, {"result", {{"kind", "created"}}}});
                return;
            }
            const auto found = _stores.find (name);
            if (found == _stores.end ())
                throw std::runtime_error ("unknown store");
            auto store = found->second;
            if (op == "dispose") {
                _stores.erase (found);
                _workers.emplace_back ([this, store, id] {
                    {
                        std::unique_lock lock (_mutex);
                        _completed.wait (lock, [&] { return store->active == 0; });
                    }
                    store->provider.reset ();
                    reply (id, {{"ok", true}, {"result", {{"kind", "disposed"}}}});
                });
                return;
            }
            if (op != "read" && op != "write" && op != "scan")
                throw std::runtime_error ("unknown operation");
            auto operation = std::make_shared<operation_t> ();
            {
                std::lock_guard lock (_mutex);
                _operations.emplace (id, operation);
                ++store->active;
            }
            _workers.emplace_back ([this, store, operation, message, id, op] {
                bool started = false;
                json body;
                try {
                    // Convert before admission so a queued cancel never calls the provider.
                    const auto request = op == "write" ? write_request (message.at ("request"))
                                                       : store_write_request_t{};
                    const auto scan =
                      op == "scan" ? scan_request (message.at ("request")) : store_scan_request_t{};
                    const auto key =
                      op == "read" ? message.at ("key").get<std::string> () : std::string ();
                    {
                        std::lock_guard lock (_mutex);
                        operation->started = !operation->cancelled;
                        started = operation->started;
                    }
                    if (!started) {
                        body = {{"ok", false},
                                {"error",
                                 {{"category", "store-failure"},
                                  {"commit", "none"},
                                  {"nativeType", "cancelled-before-start"},
                                  {"message", "Store operation stopped"}}}};
                    } else {
                        json output;
                        if (op == "read")
                            output = result (store->provider->read ({key}).result ().value ());
                        else if (op == "write")
                            output = result (store->provider->write (request).result ().value ());
                        else
                            output = result (store->provider->scan (scan).result ().value ());
                        body = {{"ok", true}, {"result", std::move (output)}};
                    }
                }
                catch (const std::exception &error) {
                    body = failure (op, error, started);
                }
                {
                    std::lock_guard lock (_mutex);
                    _operations.erase (id);
                    --store->active;
                }
                _completed.notify_all ();
                reply (id, std::move (body));
            });
        }
        catch (const std::exception &error) {
            reply (response_id, failure (response_op, error, provider));
        }
    }

    ~adapter_t ()
    {
        for (auto &worker : _workers)
            worker.join ();
    }

  private:
    void reply (const json &id, json body)
    {
        body["v"] = 1;
        body["id"] = id;
        std::lock_guard lock (_output_mutex);
        std::cout << body.dump () << '\n' << std::flush;
    }

    static json failure (const std::string &op, const std::exception &error, bool provider)
    {
        const char *category = "adapter-error";
        const char *native_type = "std::exception";
        if (provider && dynamic_cast<const std::invalid_argument *> (&error)) {
            category = "argument-error";
            native_type = "std::invalid_argument";
        } else if (provider && dynamic_cast<const framework_exception_t *> (&error)) {
            category = "store-failure";
            native_type = "framework_exception_t";
        }
        json detail{
          {"category", category}, {"nativeType", native_type}, {"message", error.what ()}};
        if (std::string_view (category) == "store-failure")
            detail["commit"] = op == "write" ? "unknown" : "none";
        return {{"ok", false}, {"error", std::move (detail)}};
    }

    std::map<std::string, std::shared_ptr<store_t>> _stores;
    std::map<std::string, std::shared_ptr<operation_t>> _operations;
    std::set<std::string> _seen;
    std::vector<std::thread> _workers;
    std::mutex _mutex;
    std::mutex _output_mutex;
    std::condition_variable _completed;
};
} // namespace

int main ()
{
    zlink::framework::completion_owner_t completion_owner;
    adapter_t adapter;
    std::string line;
    while (std::getline (std::cin, line))
        adapter.accept (line);
}
