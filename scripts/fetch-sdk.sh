#!/usr/bin/env sh
set -eu

sdk_dir="${1:-third_party/ts3client-pluginsdk}"
tag="${TS3_PLUGIN_SDK_TAG:-26}"
base_url="https://raw.githubusercontent.com/TeamSpeak-Systems/ts3client-pluginsdk/${tag}"

mkdir -p "${sdk_dir}/include/teamlog" "${sdk_dir}/include/teamspeak"

fetch_file() {
    source_path="$1"
    target_path="${sdk_dir}/${source_path}"
    curl -fsSL "${base_url}/${source_path}" -o "${target_path}"
}

fetch_file "include/plugin_definitions.h"
fetch_file "include/ts3_functions.h"
fetch_file "include/teamlog/logtypes.h"
fetch_file "include/teamspeak/public_definitions.h"
fetch_file "include/teamspeak/public_errors.h"

printf 'Fetched TeamSpeak 3 Client Plugin SDK API %s headers into %s\n' "${tag}" "${sdk_dir}"
