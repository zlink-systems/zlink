/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/pubsub_contracts.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zlink::framework::e2e::pubsub::client
{

struct client_options_t
{
    std::string scenario;
    std::string publisher_url;
    std::string subscriber_urls;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string publisher_endpoint;
    std::string log_dir;
    std::string config_dir;
    std::string subscriber_executable;
    std::string publisher_executable;
    std::string start_ready_file;
    std::string start_continue_file;
    std::string ready_file;
    std::string continue_file;
    std::string reconnect_subscriber_url;
    std::string reconnect_subscriber_pid_file;
    std::string restarted_publisher_pid_file;
};

inline client_options_t read_client_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown PubSub client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("PubSub client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open PubSub client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto value = [&] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };
    return {.scenario = value ("scenario"),
            .publisher_url = value ("publisherUrl"),
            .subscriber_urls = value ("subscriberUrls"),
            .redis_endpoint = value ("redisEndpoint"),
            .redis_key_prefix = value ("redisKeyPrefix"),
            .publisher_endpoint = value ("publisherEndpoint"),
            .log_dir = value ("logDir"),
            .config_dir = value ("configDir"),
            .subscriber_executable = value ("subscriberExecutable"),
            .publisher_executable = value ("publisherExecutable"),
            .start_ready_file = value ("startReadyFile"),
            .start_continue_file = value ("startContinueFile"),
            .ready_file = value ("readyFile"),
            .continue_file = value ("continueFile"),
            .reconnect_subscriber_url = value ("reconnectSubscriberUrl"),
            .reconnect_subscriber_pid_file = value ("reconnectSubscriberPidFile"),
            .restarted_publisher_pid_file = value ("restartedPublisherPidFile")};
}

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

inline void touch_file (const std::string &path)
{
    if (path.empty ()) {
        return;
    }
    std::ofstream file (path);
    file << "ready\n";
}

inline void wait_for_file (const std::string &path)
{
    if (path.empty ()) {
        return;
    }
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (std::filesystem::exists (path)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for " + path);
}

struct http_endpoint_t
{
    std::string host;
    std::string port;
};

inline http_endpoint_t parse_http_endpoint (const std::string &endpoint)
{
    constexpr const char *prefix = "http://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw std::runtime_error ("publisher URL must start with http://");
    }
    const auto host_start = std::string (prefix).size ();
    const auto port_separator = endpoint.find (':', host_start);
    if (port_separator == std::string::npos) {
        throw std::runtime_error ("publisher URL must include a port");
    }
    auto path_separator = endpoint.find ('/', port_separator + 1);
    if (path_separator == std::string::npos) {
        path_separator = endpoint.size ();
    }
    return {.host = endpoint.substr (host_start, port_separator - host_start),
            .port = endpoint.substr (port_separator + 1, path_separator - port_separator - 1)};
}

inline std::string url_encode (const std::string &value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
            || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back (static_cast<char> (ch));
        } else {
            encoded.push_back ('%');
            encoded.push_back (hex[ch >> 4]);
            encoded.push_back (hex[ch & 0x0F]);
        }
    }
    return encoded;
}

inline std::optional<std::string> request_text (const std::string &method,
                                                const std::string &base_url,
                                                const std::string &path_and_query,
                                                const std::string &body = {},
                                                const std::string &content_type = {},
                                                bool require_success = true)
{
    const auto endpoint = parse_http_endpoint (base_url);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *resolved = nullptr;
    if (getaddrinfo (endpoint.host.c_str (), endpoint.port.c_str (), &hints, &resolved) != 0) {
        if (!require_success) {
            return std::nullopt;
        }
        throw std::runtime_error ("failed to resolve " + endpoint.host + ":" + endpoint.port);
    }
    int socket_fd = -1;
    for (auto *address = resolved; address != nullptr; address = address->ai_next) {
        socket_fd = socket (address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (connect (socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close (socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo (resolved);
    if (socket_fd < 0) {
        if (!require_success) {
            return std::nullopt;
        }
        throw std::runtime_error ("failed to connect to publisher HTTP endpoint");
    }

    constexpr int timeout_ms = 35000;
    timeval socket_timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    (void) setsockopt (socket_fd, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout,
                       sizeof (socket_timeout));
    (void) setsockopt (socket_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout,
                       sizeof (socket_timeout));

    auto request = method + " " + path_and_query + " HTTP/1.1\r\nHost: " + endpoint.host + ":"
      + endpoint.port + "\r\nContent-Length: " + std::to_string (body.size ()) + "\r\n";
    if (!content_type.empty ()) {
        request += "Content-Type: " + content_type + "\r\n";
    }
    request += "Connection: close\r\n\r\n" + body;
    const char *cursor = request.data ();
    auto remaining = request.size ();
    while (remaining > 0) {
        const auto sent = send (socket_fd, cursor, remaining, 0);
        if (sent <= 0) {
            close (socket_fd);
            if (!require_success) {
                return std::nullopt;
            }
            throw std::runtime_error ("failed to send publisher HTTP request");
        }
        cursor += sent;
        remaining -= static_cast<std::size_t> (sent);
    }

    std::string response;
    char buffer[1024];
    while (true) {
        const auto received = recv (socket_fd, buffer, sizeof (buffer), 0);
        if (received < 0) {
            close (socket_fd);
            if (!require_success) {
                return std::nullopt;
            }
            throw std::runtime_error ("failed to read publisher HTTP response");
        }
        if (received == 0) {
            break;
        }
        response.append (buffer, static_cast<std::size_t> (received));
    }
    close (socket_fd);
    if (response.rfind ("HTTP/1.1 200", 0) != 0 && response.rfind ("HTTP/1.0 200", 0) != 0) {
        if (!require_success) {
            return std::nullopt;
        }
        throw std::runtime_error ("HTTP request failed: " + response.substr (0, 80));
    }
    const auto body_offset = response.find ("\r\n\r\n");
    if (body_offset == std::string::npos) {
        throw std::runtime_error ("HTTP response is missing its header terminator");
    }
    return response.substr (body_offset + 4);
}

inline bool request_empty (const std::string &method,
                           const std::string &base_url,
                           const std::string &path_and_query,
                           bool require_success = true)
{
    return request_text (method, base_url, path_and_query, {}, {}, require_success).has_value ();
}

inline void post_empty (const std::string &base_url, const std::string &path_and_query)
{
    (void) request_empty ("POST", base_url, path_and_query);
}

inline bool try_post_empty (const std::string &base_url, const std::string &path_and_query)
{
    return request_empty ("POST", base_url, path_and_query, false);
}

inline bool try_get (const std::string &base_url, const std::string &path)
{
    return request_empty ("GET", base_url, path, false);
}

inline void publish (const std::string &publisher_url,
                     const std::string &topic,
                     const std::string &value,
                     bool missing_packet = false)
{
    const auto path = std::string (missing_packet ? "/publish/missing" : "/publish/event")
      + "?topic=" + url_encode (topic) + "&value=" + url_encode (value);
    post_empty (publisher_url, path);
}

inline std::vector<std::string> subscriber_urls (const client_options_t &options)
{
    const auto &configured = options.subscriber_urls;
    std::vector<std::string> urls;
    std::size_t begin = 0;
    while (begin <= configured.size ()) {
        const auto separator = configured.find (',', begin);
        const auto end = separator == std::string::npos ? configured.size () : separator;
        if (end > begin) {
            urls.push_back (configured.substr (begin, end - begin));
        }
        if (separator == std::string::npos) {
            break;
        }
        begin = separator + 1;
    }
    ensure (urls.size () == 3, "subscriberUrls must contain three URLs");
    return urls;
}

inline std::vector<std::string> accepted_evidence (const std::string &value,
                                                   const std::string &topic = topic_fanout)
{
    return {"accepted|", "topic=" + topic, "value=" + value};
}

inline std::vector<std::string> ignored_evidence (const std::string &value,
                                                  const std::string &topic)
{
    return {"ignored|", "topic=" + topic, "value=" + value};
}

inline std::vector<std::string> dispatch_error_evidence (const std::string &packet,
                                                         const std::string &topic = topic_fanout)
{
    return {"error|", "kind=publish", "reason=handlerMissing", "action=drop",
            "packet=" + packet, "topic=" + topic};
}

inline std::optional<std::vector<std::string>> try_wait_for_subscriber_evidence (
  const std::string &subscriber_url, const evidence_wait_req_t &request)
{
    const auto body = request_text ("POST", subscriber_url, "/evidence/wait",
                                    nlohmann::json (request).dump (), "application/json", false);
    if (!body) {
        return std::nullopt;
    }
    return nlohmann::json::parse (*body).get<std::vector<std::string>> ();
}

inline std::vector<std::string> wait_for_subscriber_evidence (
  const std::string &subscriber_url, const evidence_wait_req_t &request)
{
    const auto lines = try_wait_for_subscriber_evidence (subscriber_url, request);
    ensure (lines.has_value (), "subscriber evidence request failed for " + subscriber_url);
    return *lines;
}

inline std::vector<std::string> wait_for_subscriber_evidence (
  const std::string &subscriber_url,
  std::vector<std::vector<std::string>> line_groups,
  int timeout_milliseconds = 10000)
{
    evidence_wait_req_t request;
    request.contains_all_line_groups = std::move (line_groups);
    request.timeout_milliseconds = timeout_milliseconds;
    return wait_for_subscriber_evidence (subscriber_url, request);
}

inline std::vector<int> common_contiguous_sequence (
  const std::vector<std::vector<std::string>> &subscriber_lines,
  int first_sequence,
  int last_sequence,
  std::string_view marker = "value=measure-")
{
    std::vector<std::vector<int>> sequences;
    sequences.reserve (subscriber_lines.size ());
    for (const auto &lines : subscriber_lines) {
        auto &sequence = sequences.emplace_back ();
        for (const auto &line : lines) {
            const auto marker_at = line.find (marker);
            if (marker_at == std::string::npos) {
                continue;
            }
            const auto value_at = marker_at + marker.size ();
            std::size_t value_end = value_at;
            while (value_end < line.size () && line[value_end] >= '0' && line[value_end] <= '9') {
                ++value_end;
            }
            if (value_end == value_at) {
                continue;
            }
            const auto value = std::stoi (line.substr (value_at, value_end - value_at));
            if (value >= first_sequence && value <= last_sequence) {
                sequence.push_back (value);
            }
        }
    }
    if (sequences.empty ()) {
        return {};
    }

    std::vector<int> longest;
    const auto &candidate_source = sequences.front ();
    for (std::size_t begin = 0; begin < candidate_source.size (); ++begin) {
        std::vector<int> candidate;
        for (std::size_t end = begin; end < candidate_source.size (); ++end) {
            if (!candidate.empty () && candidate_source[end] != candidate.back () + 1) {
                break;
            }
            candidate.push_back (candidate_source[end]);
            bool shared = true;
            for (std::size_t subscriber = 1; subscriber < sequences.size (); ++subscriber) {
                if (std::search (sequences[subscriber].begin (), sequences[subscriber].end (),
                                 candidate.begin (), candidate.end ())
                    == sequences[subscriber].end ()) {
                    shared = false;
                    break;
                }
            }
            if (shared && candidate.size () > longest.size ()) {
                longest = candidate;
            }
        }
    }
    return longest;
}

inline void ensure_no_evidence_line (const std::vector<std::string> &lines,
                                     const std::vector<std::string> &parts,
                                     const std::string &message)
{
    for (const auto &line : lines) {
        bool matched = true;
        for (const auto &part : parts) {
            if (line.find (part) == std::string::npos) {
                matched = false;
                break;
            }
        }
        ensure (!matched, message);
    }
}

inline void wait_http_health (const std::string &name, const std::string &base_url, bool expected_up)
{
    constexpr int timeout_ms = 3000;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        const bool healthy = try_get (base_url, "/health");
        if (healthy == expected_up) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for " + name
                              + (expected_up ? " health" : " shutdown"));
}

class child_process_t
{
  public:
    child_process_t () = default;
    explicit child_process_t (pid_t pid) : _pid (pid) {}

    pid_t pid () const noexcept { return _pid; }

    void terminate ()
    {
        if (_pid <= 0) {
            return;
        }
        (void) ::kill (_pid, SIGTERM);
        for (int attempt = 0; attempt < 30; ++attempt) {
            int status = 0;
            const auto waited = ::waitpid (_pid, &status, WNOHANG);
            if (waited == _pid) {
                _pid = -1;
                return;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        (void) ::kill (_pid, SIGKILL);
        (void) ::waitpid (_pid, nullptr, 0);
        _pid = -1;
    }

  private:
    pid_t _pid = -1;
};

inline void write_pid_file (const std::string &path, pid_t pid)
{
    if (path.empty () || pid <= 0) {
        return;
    }
    std::ofstream file (path);
    file << pid << "\n";
}

inline child_process_t start_process (
  const std::string &name,
  const std::string &executable,
  const std::string &config_path,
  const std::string &log_dir)
{
    ensure (!executable.empty (), "missing executable path for " + name);
    std::filesystem::create_directories (log_dir);
    const auto stdout_path = std::filesystem::path (log_dir) / (name + ".stdout.log");
    const auto stderr_path = std::filesystem::path (log_dir) / (name + ".stderr.log");
    const auto pid = ::fork ();
    if (pid < 0) {
        throw std::runtime_error ("failed to fork " + name);
    }
    if (pid == 0) {
        const int stdout_fd =
          ::open (stdout_path.c_str (), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        const int stderr_fd =
          ::open (stderr_path.c_str (), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (stdout_fd >= 0) {
            (void) ::dup2 (stdout_fd, STDOUT_FILENO);
        }
        if (stderr_fd >= 0) {
            (void) ::dup2 (stderr_fd, STDERR_FILENO);
        }
        const auto argument = "--config=" + config_path;
        ::execl (executable.c_str (), executable.c_str (), argument.c_str (),
                 static_cast<char *> (nullptr));
        _exit (127);
    }
    return child_process_t (pid);
}

inline child_process_t start_subscriber_process (const client_options_t &options,
                                                 const std::string &name,
                                                 const std::string &http_url,
                                                 const std::string &subscriber_id,
                                                 const std::string &topics,
                                                 const std::string &accepted_topics)
{
    const auto config_path = std::filesystem::path (options.config_dir) / (name + ".json");
    std::filesystem::create_directories (options.config_dir);
    std::ofstream config (config_path);
    config << nlohmann::json{{"e2e",
                              {{"subscriberId", subscriber_id},
                               {"topics", topics},
                               {"acceptedTopics", accepted_topics},
                               {"httpEndpoint", http_url},
                               {"handlerDelayMs", "0"},
                               {"redis", {{"endpoint", options.redis_endpoint},
                                          {"keyPrefix", options.redis_key_prefix}}},
                               {"logDir", options.log_dir}}}}
               .dump (2);
    config.close ();
    std::filesystem::permissions (
      config_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
    auto child = start_process (name, options.subscriber_executable, config_path.string (),
                                options.log_dir);
    wait_http_health (name, http_url, true);
    return child;
}

inline child_process_t start_publisher_process (const client_options_t &options,
                                                const std::string &name,
                                                const std::string &http_url)
{
    const auto config_path = std::filesystem::path (options.config_dir) / (name + ".json");
    std::filesystem::create_directories (options.config_dir);
    std::ofstream config (config_path);
    config << nlohmann::json{{"e2e",
                              {{"httpEndpoint", http_url},
                               {"publisherEndpoint", options.publisher_endpoint},
                               {"redis", {{"endpoint", options.redis_endpoint},
                                          {"keyPrefix", options.redis_key_prefix}}},
                               {"logDir", options.log_dir}}}}
               .dump (2);
    config.close ();
    std::filesystem::permissions (
      config_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
    auto child = start_process (name, options.publisher_executable, config_path.string (),
                                options.log_dir);
    wait_http_health (name, http_url, true);
    return child;
}

} // namespace zlink::framework::e2e::pubsub::client
