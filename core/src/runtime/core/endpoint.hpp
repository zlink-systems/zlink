/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ENDPOINT_HPP_INCLUDED__
#define __ZLINK_ENDPOINT_HPP_INCLUDED__

#include <atomic>
#include <stdint.h>
#include <string>

namespace zlink
{
class endpoint_connection_id_t
{
  public:
    explicit endpoint_connection_id_t (uint64_t value_ = 0) : _value (value_) {}

    endpoint_connection_id_t (const endpoint_connection_id_t &other_) :
        _value (other_.load ())
    {
    }

    endpoint_connection_id_t (endpoint_connection_id_t &&other_) :
        _value (other_.load ())
    {
    }

    endpoint_connection_id_t &operator= (const endpoint_connection_id_t &other_)
    {
        return operator= (other_.load ());
    }

    endpoint_connection_id_t &operator= (endpoint_connection_id_t &&other_)
    {
        return operator= (other_.load ());
    }

    endpoint_connection_id_t &operator= (uint64_t value_)
    {
        store (value_);
        return *this;
    }

    operator uint64_t () const { return load (); }

    uint64_t load () const
    {
        return _value.load (std::memory_order_acquire);
    }

    void store (uint64_t value_)
    {
        _value.store (value_, std::memory_order_release);
    }

  private:
    std::atomic<uint64_t> _value;
};

enum endpoint_type_t
{
    endpoint_type_none,   // a connection-less endpoint
    endpoint_type_bind,   // a connection-oriented bind endpoint
    endpoint_type_connect // a connection-oriented connect endpoint
};

struct endpoint_uri_pair_t
{
    endpoint_uri_pair_t ();
    endpoint_uri_pair_t (const std::string &local,
                         const std::string &remote,
                         endpoint_type_t local_type);
    endpoint_uri_pair_t (const endpoint_uri_pair_t &other_);
    endpoint_uri_pair_t (endpoint_uri_pair_t &&other_);
    endpoint_uri_pair_t &operator= (const endpoint_uri_pair_t &other_);
    endpoint_uri_pair_t &operator= (endpoint_uri_pair_t &&other_);

    const std::string &identifier () const
    {
        return local_type == endpoint_type_bind ? local : remote;
    }

    std::string local, remote;
    endpoint_type_t local_type;
    //  Process-local identity of one physical transport attempt. Copies keep
    //  the identity; each newly constructed endpoint pair receives a new one.
    endpoint_connection_id_t connection_id;

  private:
    void swap (endpoint_uri_pair_t &other_);
};

endpoint_uri_pair_t make_unconnected_connect_endpoint_pair (const std::string &endpoint_);

endpoint_uri_pair_t make_unconnected_bind_endpoint_pair (const std::string &endpoint_);

uint64_t allocate_connection_id ();
}

#endif
