/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>

namespace zlink::samples::deliverydispatch
{

/* 제안 시한은 DispatchWorker가 소유한다(공통 sample spec §7.4). 배송원의 결정을 기다리는 쪽은
 * 아무도 없다 — worker는 제안을 보내고 턴을 끝내며, 시한이 지난 제안은 sweeper가 훑어 다음
 * 배송원으로 재제안한다. 노드와 세션은 시한을 세지 않는다. */
struct sample_timings_t
{
    static constexpr std::chrono::milliseconds courier_decision_timeout{3000};
    static constexpr std::chrono::milliseconds offer_sweep_interval{100};
};

} // namespace zlink::samples::deliverydispatch
