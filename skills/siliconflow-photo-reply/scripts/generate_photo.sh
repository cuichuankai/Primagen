#!/usr/bin/env bash
set -euo pipefail

API_URL="https://api.siliconflow.cn/v1/images/generations"
SOURCE_IMAGE_URL="https://share.cheersucloud.com/apps/files_sharing/publicpreview/7XYomPrSrbrNYzZ?file=/&fileId=136606&x=1920&y=1080&a=true&etag=183eaedb94cd1a5685372f073900dc08"
STAGE="init"

on_exit() {
  local code="$?"
  if [[ "$code" -ne 0 ]]; then
    echo "Photo generation failed at stage: ${STAGE}" >&2
  fi
}
trap on_exit EXIT

if [[ -z "${SILICONFLOW_API_KEY:-}" ]]; then
  echo "SILICONFLOW_API_KEY is not set" >&2
  exit 1
fi

SCENE_CONTEXT="${*:-working out in a gym, photorealistic}"
PROMPT="make a pic of this person, but ${SCENE_CONTEXT}."

OUT_DIR="${TMPDIR:-/tmp}/primagen-photo-reply"
mkdir -p "$OUT_DIR"
OUT_FILE="${OUT_DIR}/photo_$(date +%Y%m%d_%H%M%S).png"

json_escape() {
  printf '%s' "$1" \
    | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
    | tr '\n' '\r' \
    | sed -e 's/\r/\\n/g'
}

extract_json_string() {
  local json="$1"
  local key="$2"
  local matched
  matched="$(
    printf '%s' "$json" \
      | grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" \
      | head -n1 \
      | sed -E 's/^"[^"]*"[[:space:]]*:[[:space:]]*"//; s/"$//' \
      || true
  )"
  printf '%s' "$matched"
}

json_unescape_basic() {
  printf '%s' "$1" | sed -e 's#\\/#/#g' -e 's/\\"/"/g' -e 's/\\\\/\\/g'
}

decode_base64_to_file() {
  local data="$1"
  local output="$2"
  if printf '%s' "$data" | base64 --decode >"$output" 2>/dev/null; then
    return 0
  fi
  if printf '%s' "$data" | base64 -D >"$output" 2>/dev/null; then
    return 0
  fi
  return 1
}

STAGE="build_payload"
ESCAPED_PROMPT="$(json_escape "$PROMPT")"
ESCAPED_IMAGE="$(json_escape "$SOURCE_IMAGE_URL")"
PAYLOAD="$(cat <<EOF
{"model":"Qwen/Qwen-Image-Edit-2509","prompt":"$ESCAPED_PROMPT","image_size":"1024x1024","batch_size":1,"num_inference_steps":20,"guidance_scale":7.5,"image":"$ESCAPED_IMAGE"}
EOF
)"
RESPONSE=""

request_generate_image() {
  local attempt
  local raw
  local status
  local body
  local err

  for attempt in 1 2 3; do
    STAGE="request_generation_attempt_${attempt}"
    raw="$(
      curl --silent --show-error \
        --request POST \
        --url "$API_URL" \
        --header "Authorization: Bearer ${SILICONFLOW_API_KEY}" \
        --header "Content-Type: application/json" \
        --data "$PAYLOAD" \
        --write-out $'\nHTTP_STATUS=%{http_code}'
    )"
    status="$(printf '%s' "$raw" | sed -n 's/^HTTP_STATUS=//p' | tail -n1)"
    body="$(printf '%s' "$raw" | sed '$d')"

    if [[ "$status" == "200" && -n "$body" ]]; then
      RESPONSE="$body"
      return 0
    fi

    err="$(extract_json_string "$(printf '%s' "$body" | tr -d '\r\n')" "message")"
    if [[ -z "$err" ]]; then
      err="$(extract_json_string "$(printf '%s' "$body" | tr -d '\r\n')" "error")"
    fi
    if [[ -z "$err" ]]; then
      err="Image generation failed with status ${status:-unknown}"
    fi
    echo "$err (attempt ${attempt}/3)" >&2
    sleep 1
  done

  return 1
}

if ! request_generate_image; then
  exit 1
fi

STAGE="parse_response"
COMPACT_RESPONSE="$(printf '%s' "$RESPONSE" | tr -d '\r\n')"
B64_DATA="$(extract_json_string "$COMPACT_RESPONSE" "b64_json")"
if [[ -z "$B64_DATA" ]]; then
  B64_DATA="$(extract_json_string "$COMPACT_RESPONSE" "base64")"
fi

if [[ -n "$B64_DATA" ]]; then
  STAGE="decode_base64_image"
  if decode_base64_to_file "$B64_DATA" "$OUT_FILE"; then
    echo "$OUT_FILE"
    exit 0
  fi
  echo "Failed to decode image base64 data" >&2
  exit 1
fi

IMAGE_URL="$(extract_json_string "$COMPACT_RESPONSE" "url")"
IMAGE_URL="$(json_unescape_basic "$IMAGE_URL")"
if [[ -n "$IMAGE_URL" ]]; then
  STAGE="download_image_url"
  curl --silent --show-error --fail --location "$IMAGE_URL" --output "$OUT_FILE"
  printf '%s\n' "$IMAGE_URL" > "${OUT_FILE}.url"
  echo "$OUT_FILE"
  exit 0
fi

ERROR_MESSAGE="$(extract_json_string "$COMPACT_RESPONSE" "message")"
if [[ -z "$ERROR_MESSAGE" ]]; then
  ERROR_MESSAGE="$(extract_json_string "$COMPACT_RESPONSE" "error")"
fi
if [[ -z "$ERROR_MESSAGE" ]]; then
  ERROR_MESSAGE="No image found in response"
fi
echo "$ERROR_MESSAGE" >&2
exit 1
