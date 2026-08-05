/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::samples::tictactoe
{

using namespace framework;

/* Framework host는 설정 파일 경로 하나만 받는다. 환경 변수는 읽지 않는다
 * (공통 정책 sample-e2e-configuration-policy.ko.md §2.1, §2.2). */
inline void load_sample_configuration (app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("TicTacToe role requires --config=<path>");
    }
    app.config ().load_json (*path);
}

inline sample_topology_t sample_topology_from_config (app_t &app)
{
    return app.config ().bind_required<sample_topology_t> ("sample.topology");
}

inline bool sample_keep_running (app_t &app)
{
    return app.config ().model ().get ("sample.host.keepRunning").value_or ("false") == "true";
}

} // namespace zlink::samples::tictactoe
