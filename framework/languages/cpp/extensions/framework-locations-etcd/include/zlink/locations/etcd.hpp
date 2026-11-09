/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/locations/stores.hpp>
#include <memory>

namespace zlink::framework::etcd
{
struct etcd_client_identity_t
{
    std::string certificate_pem;
    std::string key_pem;
};

struct etcd_tls_options_t
{
    std::string trusted_ca_pem;
    std::optional<etcd_client_identity_t> client_identity;
};

struct etcd_credentials_t
{
    std::string user;
    std::string password;
};

struct etcd_location_options_t
{
    std::vector<std::string> endpoints;
    std::optional<etcd_tls_options_t> tls;
    std::optional<etcd_credentials_t> credentials;
    std::string key_namespace;
    std::chrono::milliseconds operation_timeout{5000};
};

class etcd_location_store_t final : public location_store_t
{
  public:
    explicit etcd_location_store_t (etcd_location_options_t options);
    ~etcd_location_store_t () override;

    task_t<store_read_result_t> read (store_key_t key) override;
    task_t<store_write_result_t> write (store_write_request_t request) override;
    task_t<store_scan_result_t> scan (store_scan_request_t request) override;

  private:
    etcd_location_store_t (const etcd_location_store_t &) = delete;
    etcd_location_store_t &operator= (const etcd_location_store_t &) = delete;
    struct impl_t;
    std::shared_ptr<impl_t> _impl;
};
} // namespace zlink::framework::etcd
