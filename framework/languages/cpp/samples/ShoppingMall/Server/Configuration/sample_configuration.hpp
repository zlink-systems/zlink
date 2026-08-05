/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::samples::shoppingmall
{

/* role별 값. `name`이 이 프로세스가 맡은 인스턴스(api-a, workflow-b …)다. */
struct sample_role_t
{
    std::string name;
    std::string log_dir;

    static sample_role_t bind (const zlink::framework::configuration_section_t &section)
    {
        sample_role_t role;
        role.name = section.require ("name");
        role.log_dir = section.require ("logDir");
        return role;
    }
};

struct sample_configuration_t
{
    sample_topology_t topology;
    sample_role_t role;

    std::string flow_log_path () const { return role.log_dir + "/flow-" + role.name + ".log"; }
};

/* Framework host는 설정 파일 경로 하나만 받는다(공통 정책 sample-e2e-configuration-policy.ko.md
 * §2.1). 인스턴스 선택도 CLI option이 아니라 이 파일의 `sample.role.name`이 정한다. */
inline sample_configuration_t
load_sample_configuration (zlink::framework::app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("ShoppingMall role requires --config=<path>");
    }
    app.config ().load_json (*path);

    sample_configuration_t configuration;
    configuration.topology = app.config ().bind_required<sample_topology_t> ("sample.topology");
    configuration.role = app.config ().bind_required<sample_role_t> ("sample.role");
    return configuration;
}

} // namespace zlink::samples::shoppingmall
