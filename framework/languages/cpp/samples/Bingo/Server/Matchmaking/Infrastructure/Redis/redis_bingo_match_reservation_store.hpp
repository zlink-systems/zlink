/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Application/bingo_match_reservation_store.hpp"
#include "../../../Configuration/sample_topology.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <iomanip>
#include <istream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::samples::bingo
{

class redis_bingo_match_reservation_store_t final
    : public bingo_match_reservation_store_t
{
  public:
    explicit redis_bingo_match_reservation_store_t (
      const sample_topology_t &topology) :
        _topology (topology)
    {
    }

    reserve_bingo_room_res_t
    reserve (const reserve_bingo_room_req_t &request) override
    {
        if (request.mode != bingo_sample_modes_t::two_player) {
            throw std::runtime_error ("Unsupported bingo mode: " + request.mode);
        }
        if (request.actor_id.empty () || request.level_bucket.empty ()) {
            throw std::runtime_error (
              "Actor id and level bucket are required");
        }
        const auto new_room_id = make_room_id ();
        const auto settings = bingo_room_settings_payload_t{
          "Bingo Room " + new_room_id.substr (new_room_id.size () - 6),
          request.mode,
          2,
          15,
          "Game",
          ""};
        const auto now = std::to_string (std::chrono::duration_cast<std::chrono::milliseconds> (
                                           std::chrono::system_clock::now ().time_since_epoch ())
                                           .count ());
        auto reply = execute (
          {"EVAL", script (), "1", match_key (request), request.actor_id,
           new_room_id, settings.room_name, settings.mode,
           std::to_string (settings.required_players),
           std::to_string (settings.max_draw_number), settings.purpose,
           settings.observed_room_id.value_or (""), now});
        if (reply.size () != 7) {
            throw std::runtime_error ("Redis match queue returned an invalid reservation.");
        }
        return {reply[0],
                {reply[1], reply[2], std::stoi (reply[3]),
                 std::stoi (reply[4]), reply[5], reply[6]}};
    }

  private:
    std::string match_key (const reserve_bingo_room_req_t &request) const
    {
        return _topology.redis_key_prefix + "match:"
               + request.level_bucket + ":" + request.mode;
    }

    static std::string make_room_id ()
    {
        static thread_local std::mt19937_64 random{std::random_device{} ()};
        std::ostringstream value;
        value << "bingo-room-" << std::hex << std::setfill ('0')
              << std::setw (16) << random () << std::setw (16) << random ();
        return value.str ();
    }

    static std::string script ()
    {
        return R"(local key = KEYS[1]
local actorId = ARGV[1]
local newRoomId = ARGV[2]
local newRoomName = ARGV[3]
local newMode = ARGV[4]
local requiredPlayers = tonumber(ARGV[5])
local newMaxDrawNumber = ARGV[6]
local newPurpose = ARGV[7]
local newObservedRoomId = ARGV[8]
local nowMs = ARGV[9]

local function newResult()
  return { newRoomId, newRoomName, newMode, tostring(requiredPlayers),
           newMaxDrawNumber, newPurpose, newObservedRoomId }
end

local function currentResult(roomId)
  return { roomId,
           redis.call('HGET', key, 'RoomName') or '',
           redis.call('HGET', key, 'Mode') or '',
           redis.call('HGET', key, 'RequiredPlayers') or '0',
           redis.call('HGET', key, 'MaxDrawNumber') or '0',
           redis.call('HGET', key, 'Purpose') or '',
           redis.call('HGET', key, 'ObservedRoomId') or '' }
end

local roomId = redis.call('HGET', key, 'RoomId')
if not roomId then
  redis.call('HMSET', key,
    'RoomId', newRoomId,
    'RoomName', newRoomName,
    'Mode', newMode,
    'ReservedActorIds', actorId,
    'RequiredPlayers', requiredPlayers,
    'MaxDrawNumber', newMaxDrawNumber,
    'Purpose', newPurpose,
    'ObservedRoomId', newObservedRoomId,
    'CreatedAtUnixMs', nowMs)
  return newResult()
end

local actors = redis.call('HGET', key, 'ReservedActorIds') or ''
local needle = '|' .. actorId .. '|'
if string.find('|' .. actors .. '|', needle, 1, true) then
  return currentResult(roomId)
end

local count = 0
for _ in string.gmatch(actors, '[^|]+') do
  count = count + 1
end

if count >= requiredPlayers then
  redis.call('HMSET', key,
    'RoomId', newRoomId,
    'RoomName', newRoomName,
    'Mode', newMode,
    'ReservedActorIds', actorId,
    'RequiredPlayers', requiredPlayers,
    'MaxDrawNumber', newMaxDrawNumber,
    'Purpose', newPurpose,
    'ObservedRoomId', newObservedRoomId,
    'CreatedAtUnixMs', nowMs)
  return newResult()
end

if actors == '' then
  actors = actorId
else
  actors = actors .. '|' .. actorId
end
redis.call('HSET', key, 'ReservedActorIds', actors)
return currentResult(roomId))";
    }

    static std::pair<std::string, std::string> split_endpoint (std::string endpoint)
    {
        if (endpoint.rfind ("tcp://", 0) == 0) {
            endpoint = endpoint.substr (6);
        } else if (endpoint.rfind ("redis://", 0) == 0) {
            endpoint = endpoint.substr (8);
        }
        const auto separator = endpoint.rfind (':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == endpoint.size ()) {
            throw std::runtime_error ("Redis endpoint must use host:port");
        }
        return {endpoint.substr (0, separator), endpoint.substr (separator + 1)};
    }

    std::vector<std::string> execute (const std::vector<std::string> &args) const
    {
        const auto [host, port] = split_endpoint (_topology.redis_endpoint);
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver (io);
        boost::asio::ip::tcp::socket socket (io);
        boost::asio::connect (socket, resolver.resolve (host, port));
        boost::asio::write (socket, boost::asio::buffer (encode_command (args)));
        boost::asio::streambuf buffer;
        return read_array (socket, buffer);
    }

    static std::string encode_command (const std::vector<std::string> &args)
    {
        std::string command = "*" + std::to_string (args.size ()) + "\r\n";
        for (const auto &arg : args) {
            command += "$" + std::to_string (arg.size ()) + "\r\n";
            command += arg;
            command += "\r\n";
        }
        return command;
    }

    static std::string read_line (boost::asio::ip::tcp::socket &socket,
                                  boost::asio::streambuf &buffer)
    {
        boost::asio::read_until (socket, buffer, "\r\n");
        std::istream input (&buffer);
        std::string line;
        std::getline (input, line);
        if (!line.empty () && line.back () == '\r') {
            line.pop_back ();
        }
        return line;
    }

    static std::string read_bulk (boost::asio::ip::tcp::socket &socket,
                                  boost::asio::streambuf &buffer,
                                  const std::string &header)
    {
        const auto length = std::stoll (header.substr (1));
        if (length < 0) {
            return {};
        }
        while (buffer.size () < static_cast<std::size_t> (length + 2)) {
            boost::asio::read (socket, buffer, boost::asio::transfer_at_least (1));
        }
        std::string value (static_cast<std::size_t> (length), '\0');
        std::istream input (&buffer);
        input.read (value.data (), length);
        char cr = 0;
        char lf = 0;
        input.get (cr);
        input.get (lf);
        return value;
    }

    static std::vector<std::string> read_array (boost::asio::ip::tcp::socket &socket,
                                                boost::asio::streambuf &buffer)
    {
        const auto header = read_line (socket, buffer);
        if (header.empty ()) {
            throw std::runtime_error ("Redis returned an empty response");
        }
        if (header[0] == '-') {
            throw std::runtime_error ("Redis command failed: " + header.substr (1));
        }
        if (header[0] != '*') {
            throw std::runtime_error ("Redis response was not an array");
        }
        const auto count = std::stoll (header.substr (1));
        std::vector<std::string> values;
        values.reserve (static_cast<std::size_t> (count));
        for (long long index = 0; index < count; ++index) {
            const auto item = read_line (socket, buffer);
            if (item.empty () || item[0] != '$') {
                throw std::runtime_error ("Redis response item was not a bulk string");
            }
            values.push_back (read_bulk (socket, buffer, item));
        }
        return values;
    }

    sample_topology_t _topology;
};

} // namespace zlink::samples::bingo
