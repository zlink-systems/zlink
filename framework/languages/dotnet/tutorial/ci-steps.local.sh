set -euo pipefail

dotnet run --project Server/Server.csproj -c Release --no-build > server.log 2>&1 &
dotnet run --project Client/Client.csproj -c Release --no-build > client.log 2>&1 &

fail() { echo "::error::$1"; echo '--- server.log ---'; cat server.log; echo '--- client.log ---'; cat client.log; exit 1; }

# Wait until the two processes have found each other. The channel path
# and the object path come up at different times, and during startup one
# can answer while the other still refuses, so require both to succeed
# three times in a row. Every step after this runs exactly once.
# A fresh queue id each run, so a probe never reuses one whose
# earlier outcome is still cached.
warmup="warmup-$$-${RANDOM}"
streak=0
for _ in $(seq 1 90); do
  if curl -sf --max-time 10 http://127.0.0.1:5080/players/warmup/profile > /dev/null \
     && curl -sf --max-time 10 -X POST "http://127.0.0.1:5080/match-queues/${warmup}" \
          -H 'Content-Type: application/json' -d '{"playerId":"warmup"}' > /dev/null; then
    streak=$((streak + 1))
    [ "${streak}" -ge 3 ] && break
  else
    streak=0
    sleep 1
  fi
done
[ "${streak}" -ge 3 ] || fail "the two processes never connected"

# RouteMesh channel
profile=$(curl -sf http://127.0.0.1:5080/players/p1/profile)
echo "${profile}" | grep -q '"playerId":"p1"' || fail "channel request/reply: ${profile}"
curl -sf -X POST http://127.0.0.1:5080/players/p1/logins > /dev/null
grep -q 'login recorded: p1' server.log || fail "one-way channel send did not arrive"

# ClientServer channel — the caller's connection decides who answers.
ticket=$(curl -sf -X POST http://127.0.0.1:5080/players/p1/tickets)
[ "${ticket}" = '"ticket-p1"' ] || fail "client-server channel: ${ticket}"

# Fanout channel — every subscriber receives it.
curl -sf -X POST http://127.0.0.1:5080/notices \
  -H 'Content-Type: application/json' -d '{"message":"ci-broadcast"}' > /dev/null
for _ in $(seq 1 15); do
  grep -q 'maintenance notice: ci-broadcast' server.log && break
  sleep 1
done
grep -q 'maintenance notice: ci-broadcast' server.log || fail "fanout subscriber did not receive"

# User Spot — state must survive between two separate calls.
room=$(curl -sf -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"ci-room"}' | tr -d '"')
curl -sf -X POST "http://127.0.0.1:5080/rooms/${room}/chat" \
  -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}' > /dev/null
state=$(curl -sf "http://127.0.0.1:5080/rooms/${room}")
echo "${state}" | grep -q 'p1: hello' || fail "room state: ${state}"

# Instance Spot - no create call; the first message makes it. The id is
# fresh per run because a queue keeps whatever earlier runs put in it.
mode="ranked-$$-${RANDOM}"
queue=$(curl -sf -X POST "http://127.0.0.1:5080/match-queues/${mode}" \
  -H 'Content-Type: application/json' -d '{"playerId":"p1"}')
echo "${queue}" | grep -q '"waiting":1' || fail "match queue: ${queue}"
queue=$(curl -sf -X POST "http://127.0.0.1:5080/match-queues/${mode}" \
  -H 'Content-Type: application/json' -d '{"playerId":"p2"}')
echo "${queue}" | grep -q '"waiting":2' || fail "match queue state: ${queue}"

# Actor
curl -sf -X POST http://127.0.0.1:5080/players/p1 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}' > /dev/null
curl -sf -X POST http://127.0.0.1:5080/players/p1/nickname \
  -H 'Content-Type: application/json' -d '{"nickname":"rocket"}' > /dev/null
info=$(curl -sf http://127.0.0.1:5080/players/p1)
echo "${info}" | grep -q '"nickname":"rocket"' || fail "player state: ${info}"

# STREAM and session-to-player binding
stream=$(dotnet run --project StreamClient/StreamClient.csproj -c Release --no-build 2>&1)
echo "${stream}"
echo "${stream}" | grep -q 'round trip:' || fail "stream request/reply"
echo "${stream}" | grep -q 'bound player: p1' || fail "session to player binding"
echo "${stream}" | grep -q 'pushed: speedy' || fail "player push to bound session"

# Runtime status
status=$(curl -sf http://127.0.0.1:5080/status)
echo "${status}" | grep -q '"ready":true' || fail "runtime status: ${status}"

echo "all tutorial steps passed"
