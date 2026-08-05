/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::samples::tictactoe
{

/* 로그 경로는 설정 파일이 정한다(공통 정책 sample-e2e-configuration-policy.ko.md §2.3). */
inline std::string flow_log_path (const std::string &log_dir, const std::string &role)
{
    return log_dir + "/tictactoe-" + role + ".log";
}

} // namespace zlink::samples::tictactoe
