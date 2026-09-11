#!/usr/bin/env bash
# asc_testers.sh — manage TestFlight beta testers for Firefly Festival Compass
# via the App Store Connect API (no App Store Connect API client library —
# just curl + a locally-built ES256 JWT).
#
#   app/tools/asc_testers.sh list-groups
#   app/tools/asc_testers.sh ensure-group <name>
#   app/tools/asc_testers.sh add-tester <email> <first> <last> <group>
#   app/tools/asc_testers.sh list-testers <group>
#
# Environment (all required, none have defaults — this script never reads
# credentials from anywhere but the environment):
#   ASC_KEY_ID      App Store Connect API key ID (e.g. 73MN8R4WDT)
#   ASC_ISSUER_ID   App Store Connect issuer ID (UUID)
#   ASC_KEY_PATH    Path to the private key .p8 file (EC PRIME256V1 / P-256).
#                    Only its PATH is ever touched by this script — the key
#                    bytes are handed straight to `openssl dgst -sign` and are
#                    never read into a shell variable, logged, echoed, printed,
#                    copied, or written anywhere else.
# Optional:
#   ASC_APP_ID      App Store Connect app id (default: 6811207165 — Firefly
#                    Festival Compass / com.jakeholland.Firefly)
#
# Credential handling:
#   - The signed JWT (bearer token) is held only in a shell variable that is
#     never printed, logged, or included in any command's argv (it is written
#     to a curl `-K` config file with mode 600 under a private temp dir, and
#     that temp dir is removed on exit via a trap).
#   - Every function that touches the JWT wraps its body in push_notrace /
#     pop_notrace, which forces `set +x` for that block so an inherited
#     `bash -x` never prints the token.
#   - All Apple API calls go through api_request(), which uses
#     `curl -sS --fail-with-body`; on a non-2xx response only the HTTP status
#     and Apple's JSON error body are surfaced — never the request's
#     Authorization header.
#
# Dependencies: bash, curl, openssl, python3 (stdlib only — no `cryptography`
# or `pyjwt` required; this machine doesn't have either installed, so the
# ES256 signature is produced with `openssl dgst -sha256 -sign` and the DER
# signature it emits is converted to the raw r||s format JWS requires with a
# small stdlib-only python3 helper).
set -euo pipefail

ASC_API_BASE="https://api.appstoreconnect.apple.com/v1"
ASC_APP_ID="${ASC_APP_ID:-6811207165}"

# ---------------------------------------------------------------------------
# small utilities
# ---------------------------------------------------------------------------

die() {
  echo "error: $*" >&2
  exit 1
}

usage() {
  sed -n '2,29p' "$0"
}

require_env() {
  : "${ASC_KEY_ID:?ASC_KEY_ID is required}"
  : "${ASC_ISSUER_ID:?ASC_ISSUER_ID is required}"
  : "${ASC_KEY_PATH:?ASC_KEY_PATH is required}"
  [ -f "$ASC_KEY_PATH" ] || die "ASC_KEY_PATH does not point to a file: $ASC_KEY_PATH"
}

WORKDIR=""
cleanup() {
  if [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ]; then
    rm -rf "$WORKDIR"
  fi
}
trap cleanup EXIT

# Stack of prior xtrace states, so a block that must not leak the JWT under
# `bash -x` can force tracing off and reliably restore whatever it was.
_XTRACE_STACK=()
push_notrace() {
  case "$-" in
    *x*) _XTRACE_STACK+=("1") ;;
    *) _XTRACE_STACK+=("0") ;;
  esac
  set +x
}
pop_notrace() {
  local n="${#_XTRACE_STACK[@]}"
  local idx=$((n - 1))
  local was="${_XTRACE_STACK[$idx]}"
  unset "_XTRACE_STACK[$idx]"
  if [ "$was" = "1" ]; then set -x; fi
}

setup_workdir() {
  WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/asc_testers.XXXXXX")"
  chmod 700 "$WORKDIR"

  cat >"$WORKDIR/der2rs.py" <<'PYEOF'
# Convert a DER-encoded ECDSA signature (as produced by
# `openssl dgst -sha256 -sign key.p8`) on stdin into the raw, fixed-width
# r||s format that JWS/ES256 requires, on stdout. P-256 only (32-byte
# components). stdlib only.
import sys


def read_len(data, idx):
    length = data[idx]
    idx += 1
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(data[idx:idx + n], "big")
        idx += n
    return length, idx


def to_fixed(b, size=32):
    b = b.lstrip(b"\x00")
    if len(b) > size:
        raise ValueError("integer component too large for P-256")
    return b.rjust(size, b"\x00")


def main():
    data = sys.stdin.buffer.read()
    idx = 0
    if data[idx] != 0x30:
        raise ValueError("not a DER SEQUENCE")
    idx += 1
    _seq_len, idx = read_len(data, idx)
    if data[idx] != 0x02:
        raise ValueError("expected INTEGER (r)")
    idx += 1
    r_len, idx = read_len(data, idx)
    r = data[idx:idx + r_len]
    idx += r_len
    if data[idx] != 0x02:
        raise ValueError("expected INTEGER (s)")
    idx += 1
    s_len, idx = read_len(data, idx)
    s = data[idx:idx + s_len]
    idx += s_len
    sys.stdout.buffer.write(to_fixed(r) + to_fixed(s))


if __name__ == "__main__":
    main()
PYEOF

  cat >"$WORKDIR/rs2der.py" <<'PYEOF'
# Inverse of der2rs.py: raw r||s (64 bytes, P-256) on stdin -> DER on stdout.
# Only used by the offline self-test to verify a signature with
# `openssl dgst -verify`; never used on the real signing path.
import sys


def enc_int(b):
    b = b.lstrip(b"\x00")
    if not b:
        b = b"\x00"
    if b[0] & 0x80:
        b = b"\x00" + b
    return b"\x02" + bytes([len(b)]) + b


def main():
    raw = sys.stdin.buffer.read()
    r, s = raw[:32], raw[32:]
    body = enc_int(r) + enc_int(s)
    sys.stdout.buffer.write(b"\x30" + bytes([len(body)]) + body)


if __name__ == "__main__":
    main()
PYEOF

  cat >"$WORKDIR/jsonutil.py" <<'PYEOF'
# Small stdlib-only JSON helpers for asc_testers.sh. No jq / third-party deps.
import json
import sys


def load_stdin():
    return json.load(sys.stdin)


def group_id_by_name(name):
    doc = load_stdin()
    for item in doc.get("data", []):
        if item.get("attributes", {}).get("name") == name:
            print(item["id"])
            return


def single_id():
    doc = load_stdin()
    d = doc.get("data")
    if isinstance(d, dict) and "id" in d:
        print(d["id"])


def tester_id_by_email(email):
    doc = load_stdin()
    needle = email.lower()
    for item in doc.get("data", []):
        if item.get("attributes", {}).get("email", "").lower() == needle:
            print(item["id"])
            return


def print_groups_table():
    doc = load_stdin()
    rows = doc.get("data", [])
    if not rows:
        print("(no beta groups found)")
        return
    print(f"{'ID':<38} {'NAME':<24} {'INTERNAL':<9} {'PUBLIC_LINK':<12} FEEDBACK")
    for item in rows:
        a = item.get("attributes", {})
        print(
            f"{item.get('id', ''):<38} {a.get('name', ''):<24} "
            f"{str(a.get('isInternalGroup')):<9} {str(a.get('publicLinkEnabled')):<12} "
            f"{a.get('feedbackEnabled')}"
        )


def print_testers_table():
    doc = load_stdin()
    rows = doc.get("data", [])
    if not rows:
        print("(no testers found)")
        return
    print(f"{'ID':<38} {'EMAIL':<32} {'FIRST':<12} {'LAST':<12} INVITE_TYPE")
    for item in rows:
        a = item.get("attributes", {})
        print(
            f"{item.get('id', ''):<38} {a.get('email', ''):<32} "
            f"{a.get('firstName') or '':<12} {a.get('lastName') or '':<12} "
            f"{a.get('inviteType', '')}"
        )


def print_error():
    raw = sys.stdin.read()
    try:
        doc = json.loads(raw)
    except Exception:
        print(raw[:2000])
        return
    errs = doc.get("errors")
    if not errs:
        print(json.dumps(doc, indent=2)[:2000])
        return
    for e in errs:
        status = e.get("status", "")
        code = e.get("code", "")
        title = e.get("title", "")
        detail = e.get("detail", "")
        print(f"[{status} {code}] {title}: {detail}")


def has_last_name_error():
    raw = sys.stdin.read()
    sys.exit(0 if "lastname" in raw.lower() else 1)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "group_id_by_name":
        group_id_by_name(sys.argv[2])
    elif cmd == "single_id":
        single_id()
    elif cmd == "tester_id_by_email":
        tester_id_by_email(sys.argv[2])
    elif cmd == "print_groups_table":
        print_groups_table()
    elif cmd == "print_testers_table":
        print_testers_table()
    elif cmd == "print_error":
        print_error()
    elif cmd == "has_last_name_error":
        has_last_name_error()
    else:
        sys.exit(f"unknown jsonutil command: {cmd}")
PYEOF
}

b64url() {
  openssl base64 -A | tr '+/' '-_' | tr -d '='
}

urlencode() {
  python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"
}

json_str() {
  # json_str key1 val1 [key2 val2 ...] -> compact JSON object on stdout.
  # Values are always strings; omit a key entirely by not passing it.
  python3 -c '
import json, sys
args = sys.argv[1:]
obj = {}
for i in range(0, len(args), 2):
    obj[args[i]] = args[i + 1]
print(json.dumps(obj, separators=(",", ":")))
' "$@"
}

# ---------------------------------------------------------------------------
# JWT (ES256) — the only place ASC_KEY_PATH's contents are ever touched, and
# only by handing openssl the *path*; nothing here reads the key into bash.
# ---------------------------------------------------------------------------

build_jwt() {
  push_notrace
  local now exp header payload header_b64 payload_b64 signing_input sig_b64
  now=$(date -u +%s)
  exp=$((now + 1200)) # 20 minutes — Apple's documented JWT lifetime cap

  header=$(python3 -c '
import json, sys
kid = sys.argv[1]
print(json.dumps({"alg": "ES256", "kid": kid, "typ": "JWT"}, separators=(",", ":")))
' "$ASC_KEY_ID")
  payload=$(python3 -c '
import json, sys
iss, iat, exp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
print(json.dumps({"iss": iss, "iat": iat, "exp": exp, "aud": "appstoreconnect-v1"}, separators=(",", ":")))
' "$ASC_ISSUER_ID" "$now" "$exp")

  header_b64=$(printf '%s' "$header" | b64url)
  payload_b64=$(printf '%s' "$payload" | b64url)
  signing_input="${header_b64}.${payload_b64}"

  sig_b64=$(
    printf '%s' "$signing_input" \
      | openssl dgst -sha256 -sign "$ASC_KEY_PATH" \
      | python3 "$WORKDIR/der2rs.py" \
      | b64url
  )

  JWT="${signing_input}.${sig_b64}"
  pop_notrace
}

# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

# Sets API_HTTP_CODE and API_RESPONSE_FILE (a path under $WORKDIR).
api_request() {
  local method="$1" path="$2" body="${3:-}"
  build_jwt

  push_notrace
  local cfgfile bodyfile outfile code
  cfgfile="$(mktemp "$WORKDIR/curl.cfg.XXXXXX")"
  outfile="$(mktemp "$WORKDIR/resp.json.XXXXXX")"
  chmod 600 "$cfgfile" "$outfile"
  {
    printf 'header = "Authorization: Bearer %s"\n' "$JWT"
    printf 'header = "Content-Type: application/json"\n'
    printf 'header = "Accept: application/json"\n'
    printf 'silent\n'
    printf 'show-error\n'
    printf 'fail-with-body\n'
    printf 'globoff\n'
  } >"$cfgfile"
  unset JWT
  pop_notrace

  if [ -n "$body" ]; then
    bodyfile="$(mktemp "$WORKDIR/body.json.XXXXXX")"
    chmod 600 "$bodyfile"
    printf '%s' "$body" >"$bodyfile"
    code=$(curl -K "$cfgfile" -X "$method" "${ASC_API_BASE}${path}" --data-binary @"$bodyfile" -o "$outfile" -w '%{http_code}') || true
    rm -f "$bodyfile"
  else
    code=$(curl -K "$cfgfile" -X "$method" "${ASC_API_BASE}${path}" -o "$outfile" -w '%{http_code}') || true
  fi
  rm -f "$cfgfile"

  API_HTTP_CODE="$code"
  API_RESPONSE_FILE="$outfile"
}

is_api_ok() {
  [ "$API_HTTP_CODE" -ge 200 ] && [ "$API_HTTP_CODE" -lt 300 ]
}

print_api_error() {
  echo "ASC API error: HTTP ${API_HTTP_CODE}" >&2
  python3 "$WORKDIR/jsonutil.py" print_error <"$API_RESPONSE_FILE" >&2
}

check_api_ok() {
  if is_api_ok; then return 0; fi
  print_api_error
  return 1
}

# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

find_group_id() {
  local name="$1" urlenc_name
  urlenc_name=$(urlencode "$name")
  api_request GET "/betaGroups?filter[app]=${ASC_APP_ID}&filter[name]=${urlenc_name}"
  check_api_ok || return 1
  local id
  id=$(python3 "$WORKDIR/jsonutil.py" group_id_by_name "$name" <"$API_RESPONSE_FILE")
  [ -n "$id" ] || return 1
  FOUND_GROUP_ID="$id"
}

cmd_list_groups() {
  api_request GET "/betaGroups?filter[app]=${ASC_APP_ID}&limit=200"
  check_api_ok || exit 1
  python3 "$WORKDIR/jsonutil.py" print_groups_table <"$API_RESPONSE_FILE"
}

cmd_ensure_group() {
  local name="$1"
  if find_group_id "$name"; then
    echo "group '${name}' already exists: ${FOUND_GROUP_ID}"
    return 0
  fi

  local body
  body=$(python3 -c '
import json, sys
name, app_id = sys.argv[1], sys.argv[2]
print(json.dumps({
    "data": {
        "type": "betaGroups",
        "attributes": {
            "name": name,
            "isInternalGroup": False,
            "hasAccessToAllBuilds": False,
            "publicLinkEnabled": False,
            "feedbackEnabled": True,
        },
        "relationships": {
            "app": {"data": {"type": "apps", "id": app_id}}
        },
    }
}))
' "$name" "$ASC_APP_ID")

  api_request POST "/betaGroups" "$body"
  check_api_ok || exit 1
  local new_id
  new_id=$(python3 "$WORKDIR/jsonutil.py" single_id <"$API_RESPONSE_FILE")
  echo "created group '${name}': ${new_id} (feedback enabled, public link disabled)"
}

build_create_tester_body() {
  local email="$1" first="$2" last="$3" group_id="$4"
  python3 -c '
import json, sys
email, first, last, group_id = sys.argv[1:5]
print(json.dumps({
    "data": {
        "type": "betaTesters",
        "attributes": {"email": email, "firstName": first, "lastName": last},
        "relationships": {
            "betaGroups": {"data": [{"type": "betaGroups", "id": group_id}]}
        },
    }
}))
' "$email" "$first" "$last" "$group_id"
}

cmd_add_tester() {
  local email="$1" first="$2" last="$3" group_name="$4"

  find_group_id "$group_name" || die "group '${group_name}' not found; run 'ensure-group ${group_name}' first"
  local group_id="$FOUND_GROUP_ID"

  local urlenc_email
  urlenc_email=$(urlencode "$email")
  api_request GET "/betaTesters?filter[email]=${urlenc_email}"
  check_api_ok || exit 1
  local tester_id
  tester_id=$(python3 "$WORKDIR/jsonutil.py" tester_id_by_email "$email" <"$API_RESPONSE_FILE")

  if [ -n "$tester_id" ]; then
    echo "tester '${email}' already exists: ${tester_id} — linking to group '${group_name}' (${group_id})"
    local rel_body
    rel_body=$(python3 -c '
import json, sys
print(json.dumps({"data": [{"type": "betaGroups", "id": sys.argv[1]}]}))
' "$group_id")
    api_request POST "/betaTesters/${tester_id}/relationships/betaGroups" "$rel_body"
    check_api_ok || exit 1
    echo "ok: tester ${tester_id} is in group ${group_id}"
    return 0
  fi

  local last_used="$last" body
  body=$(build_create_tester_body "$email" "$first" "$last" "$group_id")
  api_request POST "/betaTesters" "$body"
  if ! is_api_ok; then
    if [ -z "$last" ] && python3 "$WORKDIR/jsonutil.py" has_last_name_error <"$API_RESPONSE_FILE"; then
      echo "note: Apple rejected an empty lastName; retrying with placeholder 'Crew'" >&2
      last_used="Crew"
      body=$(build_create_tester_body "$email" "$first" "$last_used" "$group_id")
      api_request POST "/betaTesters" "$body"
    fi
  fi
  check_api_ok || exit 1

  local new_id
  new_id=$(python3 "$WORKDIR/jsonutil.py" single_id <"$API_RESPONSE_FILE")
  echo "created tester '${email}' (${new_id}) in group '${group_name}'; lastName used: '${last_used}'"
}

cmd_list_testers() {
  local group_name="$1"
  find_group_id "$group_name" || die "group '${group_name}' not found"
  api_request GET "/betaGroups/${FOUND_GROUP_ID}/betaTesters?limit=200"
  check_api_ok || exit 1
  python3 "$WORKDIR/jsonutil.py" print_testers_table <"$API_RESPONSE_FILE"
}

# Internal / test-only: builds a JWT with whatever ASC_KEY_ID / ASC_ISSUER_ID /
# ASC_KEY_PATH are configured (a throwaway key in tests, never the real one)
# and reports structural + cryptographic validity WITHOUT ever printing the
# token, its signature, or any key material.
cmd__selftest_jwt() {
  build_jwt
  local h p s
  IFS='.' read -r h p s <<<"$JWT"

  local header_json payload_json
  header_json=$(python3 -c '
import base64, sys
s = sys.argv[1].replace("-", "+").replace("_", "/")
print(base64.b64decode(s + "=" * (-len(s) % 4)).decode())
' "$h")
  payload_json=$(python3 -c '
import base64, sys
s = sys.argv[1].replace("-", "+").replace("_", "/")
print(base64.b64decode(s + "=" * (-len(s) % 4)).decode())
' "$p")

  echo "header: ${header_json}"
  echo "payload: ${payload_json}"
  echo "jwt_length_chars: ${#JWT}"

  local pubkey sig_der signing_input
  pubkey="$WORKDIR/pub.pem"
  sig_der="$WORKDIR/sig.der"
  signing_input="$WORKDIR/signing_input.txt"
  openssl ec -in "$ASC_KEY_PATH" -pubout -out "$pubkey" 2>/dev/null
  python3 -c '
import base64, sys
s = sys.argv[1].replace("-", "+").replace("_", "/")
sys.stdout.buffer.write(base64.b64decode(s + "=" * (-len(s) % 4)))
' "$s" | python3 "$WORKDIR/rs2der.py" >"$sig_der"
  printf '%s.%s' "$h" "$p" >"$signing_input"

  if openssl dgst -sha256 -verify "$pubkey" -signature "$sig_der" "$signing_input" 2>/dev/null | grep -q "Verified OK"; then
    echo "signature_valid: true"
  else
    echo "signature_valid: false"
    unset JWT
    return 1
  fi
  unset JWT
}

main() {
  local cmd="${1:-}"
  case "$cmd" in
    list-groups)
      require_env
      setup_workdir
      cmd_list_groups
      ;;
    ensure-group)
      shift
      [ $# -ge 1 ] || die "usage: $0 ensure-group <name>"
      require_env
      setup_workdir
      cmd_ensure_group "$1"
      ;;
    add-tester)
      shift
      [ $# -ge 4 ] || die "usage: $0 add-tester <email> <first> <last> <group>"
      require_env
      setup_workdir
      cmd_add_tester "$1" "$2" "$3" "$4"
      ;;
    list-testers)
      shift
      [ $# -ge 1 ] || die "usage: $0 list-testers <group>"
      require_env
      setup_workdir
      cmd_list_testers "$1"
      ;;
    _selftest-jwt)
      # internal: see cmd__selftest_jwt above. Requires ASC_KEY_ID /
      # ASC_ISSUER_ID / ASC_KEY_PATH — intended for a throwaway key only.
      require_env
      setup_workdir
      cmd__selftest_jwt
      ;;
    -h | --help | help | "")
      usage
      ;;
    *)
      die "unknown command: ${cmd} (see --help)"
      ;;
  esac
}

main "$@"
