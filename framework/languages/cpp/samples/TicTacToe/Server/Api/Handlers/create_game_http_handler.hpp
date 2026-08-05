/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Configuration/sample_names.hpp"
#include "../../Configuration/sample_topology.hpp"

#include <zlink/framework.hpp>

namespace zlink::samples::tictactoe
{

using namespace framework;

class create_game_http_handler_t
{
  public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types = dependency_list_t<spot_manager_t,
                                               sample_topology_t,
                                               logger_t<create_game_http_handler_t>>;
    static constexpr const char *topic_name = "CreateGame";

    explicit create_game_http_handler_t (spot_manager_t &spots,
                                         sample_topology_t &topology,
                                         logger_t<create_game_http_handler_t> &logger) :
        _spots (spots), _topology (topology), _logger (logger)
    {
    }

    task_t<create_game_http_res_t> handle (const create_game_http_req_t &request)
    {
        const auto game_name = request.game_name.value_or ("tictactoe-game");
        _logger.info ("http POST /games");
        _logger.info (std::string ("recv ") + create_game_http_req_t::packet_name);
        // --8<-- [start:doc-create]
        // co_await는 coroutine 안에서만 쓴다. 이 handler의 반환형이 task_t인 이유다.
        auto room = co_await _spots.create (sample_names_t::match_spot) // 이 stable type을 등록한 node가 후보가 된다.
                      .in_mesh (sample_names_t::game_spot_node)         // Spot을 만들 mesh를 고른다.
                      .creation_request (                               // 새 Spot의 생성 callback에 전달할 최초 설정이다.
                        create_game_req_t{game_name, sample_names_t::required_level})
                      .submit ();                                       // C++의 비동기 완료 terminal이다.
        // --8<-- [end:doc-create]
        _logger.info (std::string ("reply ") + create_game_http_res_t::packet_name);
        co_return create_game_http_res_t{
          room.spot.spot_id (),
          game_name,
          {_topology.play_a_stream_endpoint, _topology.play_b_stream_endpoint},
          {{_topology.play_a_stream_endpoint}, {_topology.play_b_stream_endpoint}},
          sample_names_t::required_level};
    }

  private:
    spot_manager_t &_spots;
    sample_topology_t &_topology;
    logger_t<create_game_http_handler_t> _logger;
};

} // namespace zlink::samples::tictactoe
