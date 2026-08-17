#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

for host in Server/CommerceApi/src/main/kotlin Server/OrderWorkflow/src/main/kotlin; do
  if ! rg -q 'useCoroutineHandlers\(Dispatchers\.Default\)' "${host}" --glob '*.kt'; then
    echo "ShoppingMall framework host must configure coroutine handlers: ${host}" >&2
    exit 1
  fi
done
if rg -n 'LockSupport\.parkNanos' \
    Client/src/main/kotlin \
    Server/CommerceApi/src/main/kotlin --glob '*.kt'; then
  echo "ShoppingMall suspend paths must not block coroutine threads" >&2
  exit 1
fi

source "../../runner-common.sh"
zlink_sample_configure_port_pool kotlin
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

pids=()
redis_container_id=""
log_dir="build/sample-logs"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="workflow-a.log workflow-b.log api-a.log api-b.log"
store_dir="build/sample-store"
config_dir="build/sample-config"
export SHOPPINGMALL_LOG_DIR="${SHOPPINGMALL_LOG_DIR:-$(pwd)/logs}"
mkdir -p "${log_dir}" "${store_dir}" "${config_dir}" "${SHOPPINGMALL_LOG_DIR}"
rm -f "${log_dir}"/*.log
rm -f "${SHOPPINGMALL_LOG_DIR}"/*.log
rm -f "${store_dir}"/*
rm -f "${config_dir}"/*.properties

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 200 "${log}" >&2 || true
  done
}

trap cleanup EXIT

build_framework_jars() {
  (
    cd ../../..
    zlink_sample_gradle_locked ./gradlew --no-daemon \
      --no-parallel \
      --max-workers=1 \
      :zlink-framework-core:jar \
      :zlink-framework-spring-boot-starter:jar \
      :zlink-framework-kotlin:jar \
      :zlink-framework-locations-redis:jar \
      --quiet
  )
}

read -r -a reserved_endpoints <<<"$(zlink_sample_reserve_endpoints 4)"
if [[ "${#reserved_endpoints[@]}" -ne 4 ]]; then
  echo "Failed to reserve sample ports." >&2
  exit 1
fi
commerce_a="${reserved_endpoints[0]}"
commerce_b="${reserved_endpoints[1]}"
workflow_a="${reserved_endpoints[2]}"
workflow_b="${reserved_endpoints[3]}"
commerce_a_host="${commerce_a%:*}"; commerce_a_port="${commerce_a##*:}"
commerce_b_host="${commerce_b%:*}"; commerce_b_port="${commerce_b##*:}"
workflow_a_host="${workflow_a%:*}"; workflow_a_port="${workflow_a##*:}"
workflow_b_host="${workflow_b%:*}"; workflow_b_port="${workflow_b##*:}"

shoppingmall_redis_key_prefix="${SHOPPINGMALL_REDIS_KEY_PREFIX:-shoppingmall:kotlin:${RANDOM}:$$:}"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-shoppingmall" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
SHOPPINGMALL_REDIS_ENDPOINT="127.0.0.1:${redis_port}"
wait_port "${SHOPPINGMALL_REDIS_ENDPOINT%:*}" "${SHOPPINGMALL_REDIS_ENDPOINT##*:}"

write_role_config() {
  local path="$1" instance_id="$2" channel_endpoint="$3"
  cat >"${path}" <<EOF
sample.instanceId=${instance_id}
sample.logDirectory=${SHOPPINGMALL_LOG_DIR}
sample.channelEndpoint=${channel_endpoint}
sample.redisEndpoint=${SHOPPINGMALL_REDIS_ENDPOINT}
sample.redisKeyPrefix=${shoppingmall_redis_key_prefix}
sample.storeDirectory=${PWD}/${store_dir}
EOF
}

workflow_a_config="${config_dir}/workflow-a.properties"
workflow_b_config="${config_dir}/workflow-b.properties"
api_a_config="${config_dir}/api-a.properties"
api_b_config="${config_dir}/api-b.properties"
client_config="${config_dir}/client.properties"
write_role_config "${workflow_a_config}" workflow-a "tcp://${workflow_a_host}:${workflow_a_port}"
write_role_config "${workflow_b_config}" workflow-b "tcp://${workflow_b_host}:${workflow_b_port}"
write_role_config "${api_a_config}" api-a "tcp://${commerce_a_host}:${commerce_a_port}"
write_role_config "${api_b_config}" api-b "tcp://${commerce_b_host}:${commerce_b_port}"
write_role_config "${client_config}" client "tcp://127.0.0.1:1"

build_framework_jars
gradle_run \
  :Server:OrderWorkflow:installDist \
  :Server:CommerceApi:installDist \
  :Client:installDist

"$(app_bin Server/OrderWorkflow OrderWorkflow)" --config "${workflow_a_config}" >"${log_dir}/workflow-a.log" 2>&1 &
pids+=("$!")
wait_port "${workflow_a_host}" "${workflow_a_port}"

"$(app_bin Server/OrderWorkflow OrderWorkflow)" --config "${workflow_b_config}" >"${log_dir}/workflow-b.log" 2>&1 &
pids+=("$!")
wait_port "${workflow_b_host}" "${workflow_b_port}"

"$(app_bin Server/CommerceApi CommerceApi)" --config "${api_a_config}" >"${log_dir}/api-a.log" 2>&1 &
pids+=("$!")
wait_port "${commerce_a_host}" "${commerce_a_port}"

"$(app_bin Server/CommerceApi CommerceApi)" --config "${api_b_config}" >"${log_dir}/api-b.log" 2>&1 &
pids+=("$!")
wait_port "${commerce_b_host}" "${commerce_b_port}"

"$(app_bin Client Client)" --config "${client_config}" >"${log_dir}/client.log" 2>&1

grep -q "shoppingmall order: started" "${log_dir}/workflow-a.log"
grep -q "shoppingmall order: started" "${log_dir}/workflow-b.log"
grep -q "shoppingmall evidence:" "${log_dir}/api-a.log"
grep -Eq "zlink flow:" "${log_dir}"/{api,workflow}-*.log
echo "shoppingmall-server-evidence=completed"
