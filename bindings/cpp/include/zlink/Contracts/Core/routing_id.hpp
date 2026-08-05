/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <chrono>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <stdexcept>

namespace zlink
{

/// @brief Identifies a socket's messaging pattern.
enum class socket_type : int
{
    any = 0, ///< Any type (filter value, not a real socket type).
    pair = 0x1001,
    pub = 0x1002,
    sub = 0x1003,
    dealer = 0x1004,
    router = 0x1005,
    xpub = 0x1006,
    xsub = 0x1007,
    stream = 0x1008
};

/// @brief Context configuration option keys.
enum class context_option : int
{
    io_threads = 1,
    max_sockets = 2,
    socket_limit = 3,
    thread_priority = 3,
    thread_sched_policy = 4,
    max_msgsz = 5,
    msg_t_size = 6,
    thread_affinity_cpu_add = 7,
    thread_affinity_cpu_remove = 8,
    thread_name_prefix = 9,
    blocky = 10,
    auto_hwm_enable = 12,
    auto_hwm_recalc_debounce_ms = 14,
    auto_hwm_profile = 17,
    auto_hwm_msg_unit_bytes = 18
};

/// @brief Selects an automatic high-water-mark sizing profile.
enum class auto_hwm_profile : int
{
    compact = 0,     ///< Smallest queues, minimizing memory use.
    low_latency = 1, ///< Small queues that drain quickly to favor latency.
    balanced = 2,    ///< Balances latency against throughput.
    throughput = 3   ///< Large queues that favor throughput.
};

/// @brief OS scheduling policy for I/O threads.
enum class thread_scheduling_policy_t : int
{
    default_policy = -1,
    other = 0,
    fifo = 1,
    round_robin = 2
};

/// @brief How a socket reacts to a peer that reuses an existing routing id.
enum class rid_duplicate_policy_t : int
{
    reject = 0,   ///< Reject the new peer and keep the existing route.
    handover = 1  ///< Hand the routing id to the new peer, dropping the previous holder.
};

class routing_id_t;
class received_t;
class topic_message_t;
class socket_t;
class pair_socket_t;
class dealer_socket_t;
class router_socket_t;
class stream_socket_t;
class pub_socket_t;
class xpub_socket_t;
class timer_t;
class send_operation_t;
class send_submit_operation_t;
class request_operation_t;
class request_submit_operation_t;
class request_callback_submit_operation_t;
class reply_operation_t;
class reply_submit_operation_t;
/// @brief Strongly-typed I/O thread count for context creation.
class io_thread_count_t
{
  public:
    static io_thread_count_t value (int value_) noexcept { return io_thread_count_t (value_); }

    int value () const noexcept { return _value; }

  private:
    explicit io_thread_count_t (int value_) noexcept : _value (value_) {}

    int _value;
};

class socket_count_t
{
  public:
    static socket_count_t value (int value_) noexcept { return socket_count_t (value_); }

    int value () const noexcept { return _value; }

  private:
    explicit socket_count_t (int value_) noexcept : _value (value_) {}

    int _value;
};

class worker_count_t
{
  public:
    static worker_count_t value (int value_) noexcept { return worker_count_t (value_); }

    int value () const noexcept { return _value; }

  private:
    explicit worker_count_t (int value_) noexcept : _value (value_) {}

    int _value;
};

class thread_priority_t
{
  public:
    static thread_priority_t value (int value_) noexcept { return thread_priority_t (value_); }

    int value () const noexcept { return _value; }

  private:
    explicit thread_priority_t (int value_) noexcept : _value (value_) {}

    int _value;
};

class cpu_index_t
{
  public:
    static cpu_index_t value (int value_) noexcept { return cpu_index_t (value_); }

    int value () const noexcept { return _value; }

  private:
    explicit cpu_index_t (int value_) noexcept : _value (value_) {}

    int _value;
};

class byte_size_t
{
  public:
    static byte_size_t bytes (int64_t value_) noexcept { return byte_size_t (value_); }

    int64_t bytes () const noexcept { return _bytes; }

  private:
    explicit byte_size_t (int64_t value_) noexcept : _bytes (value_) {}

    int64_t _bytes;
};

class peer_weight_t
{
  public:
    static peer_weight_t value (uint32_t value_)
    {
        if (value_ > 100u)
            throw std::invalid_argument ("peer_weight_t must be in range 0..100");
        return peer_weight_t (value_);
    }

    uint32_t value () const noexcept { return _value; }

  private:
    explicit peer_weight_t (uint32_t value_) noexcept : _value (value_) {}

    uint32_t _value;
};

class socket_backlog_t
{
  public:
    static socket_backlog_t value (int value_) noexcept { return socket_backlog_t (value_); }

    int value () const noexcept { return _value; }

  private:
    explicit socket_backlog_t (int value_) noexcept : _value (value_) {}

    int _value;
};

namespace detail
{
struct routing_id_access_t;
inline routing_id_t unchecked_empty_routing_id () noexcept;
inline bool routing_id_empty (const routing_id_t &routing_id_) noexcept;

inline void validate_no_embedded_null (const std::string &value_, const char *field_name_)
{
    if (value_.find ('\0') != std::string::npos)
        throw std::invalid_argument (std::string (field_name_) + " must not contain embedded null");
}

inline void
validate_bounded_c_string (const std::string &value_, size_t max_bytes_, const char *field_name_)
{
    validate_no_embedded_null (value_, field_name_);
    if (value_.size () > max_bytes_) {
        throw std::invalid_argument (std::string (field_name_) + " exceeds "
                                     + std::to_string (max_bytes_) + " bytes");
    }
}

} // namespace detail

/// @brief An opaque identifier for a messaging peer or route, 1 to 255 bytes.
class routing_id_t
{
  public:
    routing_id_t (const uint8_t *bytes_, size_t size_) : _data (), _size (0)
    {
        assign (bytes_, size_);
    }

    routing_id_t (const routing_id_t &other_) = default;

    routing_id_t &operator= (const routing_id_t &other_) = default;

    routing_id_t (routing_id_t &&other_) noexcept = default;

    routing_id_t &operator= (routing_id_t &&other_) noexcept = default;

    static routing_id_t from (const uint8_t *bytes_, size_t size_)
    {
        return routing_id_t (bytes_, size_);
    }

    static routing_id_t from (const std::vector<uint8_t> &bytes_)
    {
        return routing_id_t (bytes_.empty () ? nullptr : bytes_.data (), bytes_.size ());
    }

    static routing_id_t from (const std::string &value_)
    {
        return routing_id_t (reinterpret_cast<const uint8_t *> (value_.data ()), value_.size ());
    }

    static routing_id_t from (uint32_t value_)
    {
        const uint8_t bytes[4] = {static_cast<uint8_t> ((value_ >> 24u) & 0xffu),
                                  static_cast<uint8_t> ((value_ >> 16u) & 0xffu),
                                  static_cast<uint8_t> ((value_ >> 8u) & 0xffu),
                                  static_cast<uint8_t> (value_ & 0xffu)};
        return from (bytes, sizeof (bytes));
    }

    static routing_id_t from (const std::array<uint8_t, 16> &value_)
    {
        return from (value_.data (), value_.size ());
    }

    static routing_id_t from_hex (const std::string &value_)
    {
        if (value_.empty () || (value_.size () % 2u) != 0u)
            throw std::invalid_argument ("routing id string must be hex");
        if (value_.size () > 510u)
            throw std::invalid_argument ("routing id string must decode to at most 255 bytes");

        std::vector<uint8_t> bytes;
        bytes.reserve (value_.size () / 2u);
        for (size_t i = 0; i < value_.size (); i += 2u) {
            const int high = hex_nibble (value_[i]);
            const int low = hex_nibble (value_[i + 1u]);
            if (high < 0 || low < 0)
                throw std::invalid_argument ("routing id string must be hex");
            bytes.push_back (static_cast<uint8_t> ((high << 4) | low));
        }
        return from (bytes);
    }

    const uint8_t *data () const noexcept { return _data.data (); }
    size_t size () const noexcept { return _size; }

    std::vector<uint8_t> to_bytes () const
    {
        return std::vector<uint8_t> (_data.data (), _data.data () + _size);
    }

    std::string to_string () const
    {
        if (is_printable_utf8 ()) {
            return std::string (reinterpret_cast<const char *> (data ()), size ());
        }
        if (size () == 4u) {
            const uint8_t *bytes = data ();
            const uint32_t value =
              (static_cast<uint32_t> (bytes[0]) << 24u) | (static_cast<uint32_t> (bytes[1]) << 16u)
              | (static_cast<uint32_t> (bytes[2]) << 8u) | static_cast<uint32_t> (bytes[3]);
            return std::to_string (value);
        }
        if (size () == 16u) {
            const uint8_t *bytes = data ();
            char out[37];
            std::snprintf (out, sizeof (out),
                           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                           bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                           bytes[14], bytes[15]);
            return std::string (out);
        }
        return std::string ("hex:") + to_hex ();
    }

    std::string to_hex () const
    {
        static const char *digits = "0123456789abcdef";
        std::string hex;
        hex.resize (size () * 2u);
        for (size_t i = 0; i < size (); ++i) {
            const uint8_t byte = _data[i];
            hex[(i * 2u)] = digits[(byte >> 4u) & 0x0fu];
            hex[(i * 2u) + 1u] = digits[byte & 0x0fu];
        }
        return hex;
    }

    friend bool operator== (const routing_id_t &a_, const routing_id_t &b_) noexcept
    {
        return a_._size == b_._size
               && std::memcmp (a_._data.data (), b_._data.data (), a_._size) == 0;
    }

    friend bool operator!= (const routing_id_t &a_, const routing_id_t &b_) noexcept
    {
        return !(a_ == b_);
    }

  private:
    routing_id_t () noexcept : _data (), _size (0) {}

    void assign (const uint8_t *bytes_, size_t size_)
    {
        if (size_ == 0)
            throw std::invalid_argument ("routing id must not be empty");
        if (size_ > _data.size ())
            throw std::invalid_argument ("routing id exceeds 255 bytes");
        if (size_ > 0 && !bytes_)
            throw std::invalid_argument ("routing id bytes must not be null for non-empty input");

        _data.fill (0);
        _size = static_cast<uint8_t> (size_);
        if (size_ > 0)
            std::memcpy (_data.data (), bytes_, size_);
    }

    void assign_unchecked (const uint8_t *bytes_, size_t size_) noexcept
    {
        _data.fill (0);
        _size = static_cast<uint8_t> (size_);
        if (size_ > 0 && bytes_)
            std::memcpy (_data.data (), bytes_, size_);
    }

    std::array<uint8_t, 255> _data;
    uint8_t _size;

    static int hex_nibble (char value_) noexcept
    {
        if (value_ >= '0' && value_ <= '9')
            return value_ - '0';
        if (value_ >= 'a' && value_ <= 'f')
            return value_ - 'a' + 10;
        if (value_ >= 'A' && value_ <= 'F')
            return value_ - 'A' + 10;
        return -1;
    }

    bool is_printable_utf8 () const noexcept
    {
        const uint8_t *bytes = data ();
        const size_t n = size ();
        for (size_t i = 0; i < n;) {
            const uint8_t c = bytes[i];
            if (c < 0x20u || c == 0x7fu)
                return false;
            if (c < 0x80u) {
                ++i;
                continue;
            }
            size_t need = 0;
            if ((c & 0xe0u) == 0xc0u) {
                if (c < 0xc2u)
                    return false;
                need = 1;
            } else if ((c & 0xf0u) == 0xe0u) {
                need = 2;
            } else if ((c & 0xf8u) == 0xf0u) {
                if (c > 0xf4u)
                    return false;
                need = 3;
            } else {
                return false;
            }
            if (i + need >= n)
                return false;
            for (size_t j = 1; j <= need; ++j) {
                if ((bytes[i + j] & 0xc0u) != 0x80u)
                    return false;
            }
            i += need + 1u;
        }
        return true;
    }

    friend inline routing_id_t detail::unchecked_empty_routing_id () noexcept;
    friend inline bool detail::routing_id_empty (const routing_id_t &) noexcept;
    friend struct detail::routing_id_access_t;
};

namespace detail
{

inline routing_id_t unchecked_empty_routing_id () noexcept
{
    return routing_id_t ();
}

inline bool routing_id_empty (const routing_id_t &routing_id_) noexcept
{
    return routing_id_._size == 0;
}

} // namespace detail

template <size_t N> inline std::string fixed_string_to_string (const char (&src_)[N]);

template <size_t N> inline std::string fixed_string_to_string (const char (&src_)[N])
{
    size_t len = 0;
    while (len < N && src_[len] != '\0')
        ++len;
    return std::string (src_, len);
}

} // namespace zlink

namespace std
{

template <> struct hash<zlink::routing_id_t>
{
    size_t operator() (const zlink::routing_id_t &rid_) const noexcept
    {
        size_t seed = 1469598103934665603ull;
        const uint8_t *data = rid_.data ();
        for (size_t i = 0; i < rid_.size (); ++i) {
            seed ^= static_cast<size_t> (data[i]);
            seed *= 1099511628211ull;
        }
        return seed;
    }
};

} // namespace std
