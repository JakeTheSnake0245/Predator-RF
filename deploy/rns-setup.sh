#!/usr/bin/env bash
# ============================================================
# Predator RF — RNS daemon setup / replication helper.
#
#   sudo ./deploy/rns-setup.sh                # interactive install
#   sudo ./deploy/rns-setup.sh --replicate    # mint a token for the next node
#   sudo ./deploy/rns-setup.sh --non-interactive  # consumes env vars
#
# Env (used by --non-interactive):
#   PRF_RNS_TOKEN          — full prf-rns-v1.* token to import
#   PRF_RNS_PASSPHRASE     — passphrase for the token
#   PRF_RNS_PLACEHOLDER__<encoded_path>=<value>
#       Encode the dotted placeholder path with `__` for each `.` so
#       paths containing literal underscores survive intact, e.g.
#         interfaces.0.listen_address  →
#         PRF_RNS_PLACEHOLDER__interfaces__0__listen_address=0.0.0.0
#   PRF_RNS_PLACEHOLDERS_JSON — JSON object of {"path": "value"}.
#       Always wins over the per-var form above.
#   PRF_RNS_NEW_PASSPHRASE — for --replicate
# ============================================================
set -euo pipefail

INSTALL_DIR="${PRF_INSTALL_DIR:-/opt/predator-rf}"
STATE_DIR="${PRF_RNS_STATE_DIR:-/var/lib/predator-rns}"
PY="${PRF_PYTHON:-python3}"
SVC_USER="${PRF_RNS_USER:-predator}"
MODE="install"
NON_INTERACTIVE=0

for arg in "$@"; do
  case "$arg" in
    --replicate)        MODE="replicate" ;;
    --non-interactive)  NON_INTERACTIVE=1 ;;
    -h|--help)
      sed -n '2,15p' "$0"
      exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "must run as root (sudo)" >&2
  exit 1
fi

# ── shared helpers ─────────────────────────────────────────────────────

run_py() {
  PYTHONPATH="${INSTALL_DIR}" "${PY}" -c "$1"
}

ensure_state_dir() {
  install -d -o "${SVC_USER}" -g "${SVC_USER}" -m 0700 "${STATE_DIR}"
}

ensure_user() {
  id -u "${SVC_USER}" &>/dev/null || \
    useradd --system --no-create-home --shell /usr/sbin/nologin "${SVC_USER}"
}

apt_deps() {
  apt-get update -qq
  apt-get install -y --no-install-recommends \
    python3 python3-venv python3-pip git ca-certificates
}

install_pkg() {
  if [[ -f "${INSTALL_DIR}/backend/rns/requirements.txt" ]]; then
    "${PY}" -m pip install --quiet -r \
      "${INSTALL_DIR}/backend/rns/requirements.txt"
  else
    "${PY}" -m pip install --quiet rns cbor2 argon2-cffi pynacl
  fi
}

prompt_or_env() {
  # $1=var, $2=prompt, $3=secret(0|1)
  local v="${!1:-}"
  if [[ -n "$v" ]]; then echo "$v"; return; fi
  if [[ "$NON_INTERACTIVE" -eq 1 ]]; then
    echo "missing required env var $1" >&2; exit 3
  fi
  if [[ "$3" == "1" ]]; then
    read -rsp "$2: " v; echo "" >&2
  else
    read -rp "$2: " v
  fi
  echo "$v"
}

collect_placeholders_to_json() {
  # Reads PRF_RNS_PLACEHOLDER__* env vars (encoding `__` → `.`) and
  # PRF_RNS_PLACEHOLDERS_JSON, merging into a single JSON object.
  # `__`→`.` is reversible for any path that doesn't itself contain
  # `__`, which the schema never does.
  "${PY}" -c '
import json, os
out = {}
extra = os.environ.get("PRF_RNS_PLACEHOLDERS_JSON")
if extra:
    try:
        for k, v in (json.loads(extra) or {}).items():
            out[str(k)] = v
    except Exception:
        pass
for k, v in os.environ.items():
    if k.startswith("PRF_RNS_PLACEHOLDER__"):
        raw = k[len("PRF_RNS_PLACEHOLDER__"):]
        path = raw.replace("__", ".")
        out[path] = v
print(json.dumps(out))
'
}

# ── replicate mode ─────────────────────────────────────────────────────

if [[ "$MODE" == "replicate" ]]; then
  ensure_state_dir
  NEW_PASS="$(prompt_or_env PRF_RNS_NEW_PASSPHRASE \
    'New passphrase for the replication token' 1)"
  TOKEN="$(PYTHONPATH="${INSTALL_DIR}" \
    PREDATOR_RNS_STATE_DIR="${STATE_DIR}" \
    "${PY}" - <<PY
from backend.rns.daemon import RNSDaemon
d = RNSDaemon()
print(d.mint_replication_token("$NEW_PASS", include_identity=False)["token"])
PY
)"
  echo
  echo "REPLICATION TOKEN (hand to next node along with the passphrase):"
  echo "$TOKEN"
  if [[ -t 1 ]]; then
    if command -v qrencode >/dev/null 2>&1; then
      echo
      qrencode -t ANSIUTF8 "$TOKEN"
    else
      echo "(install 'qrencode' for a terminal QR rendering)"
    fi
  fi
  exit 0
fi

# ── install mode ───────────────────────────────────────────────────────

echo "[1/6] apt deps"
apt_deps

echo "[2/6] system user + state dir"
ensure_user
ensure_state_dir

echo "[3/6] python deps"
install_pkg

echo "[4/6] systemd unit"
if [[ -f "${INSTALL_DIR}/deploy/predator-rns.service" ]]; then
  install -m 0644 "${INSTALL_DIR}/deploy/predator-rns.service" \
    /etc/systemd/system/predator-rns.service
  systemctl daemon-reload
fi

echo "[5/6] config import"
TOKEN="$(prompt_or_env PRF_RNS_TOKEN \
  'Paste the prf-rns-v1.* token (or empty to skip import)' 0)"
if [[ -n "$TOKEN" ]]; then
  PASS="$(prompt_or_env PRF_RNS_PASSPHRASE 'Token passphrase' 1)"
  PH_JSON="$(collect_placeholders_to_json)"
  # Up to 5 import attempts: if the daemon reports missing
  # placeholders, prompt the operator for each and retry. In
  # --non-interactive mode we fail fast with a clear list.
  attempt=0
  while : ; do
    attempt=$((attempt+1))
    RESULT="$(PYTHONPATH="${INSTALL_DIR}" \
      PREDATOR_RNS_STATE_DIR="${STATE_DIR}" \
      PRF_RNS_TOKEN_IN="$TOKEN" PRF_RNS_PASS_IN="$PASS" \
      PRF_RNS_PH_IN="$PH_JSON" "${PY}" - <<'PY'
import json, os, sys
from backend.rns.daemon import RNSDaemon
d = RNSDaemon()
res = d.import_config(os.environ["PRF_RNS_TOKEN_IN"],
                      os.environ["PRF_RNS_PASS_IN"],
                      placeholders=json.loads(os.environ["PRF_RNS_PH_IN"]))
print(json.dumps(res))
PY
)"
    APPLIED="$(echo "$RESULT" | "${PY}" -c \
      'import sys,json;print(json.load(sys.stdin)["applied"])')"
    if [[ "$APPLIED" == "True" ]]; then
      echo "config imported"
      break
    fi
    MISSING="$(echo "$RESULT" | "${PY}" -c \
      'import sys,json;print("\n".join(json.load(sys.stdin)["missing_placeholders"]))')"
    if [[ "$NON_INTERACTIVE" -eq 1 ]]; then
      echo "MISSING placeholders — supply via "\
"PRF_RNS_PLACEHOLDER__<encoded_path>=<value> (use __ for .) "\
"or PRF_RNS_PLACEHOLDERS_JSON:" >&2
      echo "$MISSING" | sed 's/^/  /' >&2
      exit 4
    fi
    if [[ $attempt -gt 5 ]]; then
      echo "too many import attempts — aborting" >&2
      exit 4
    fi
    echo "Token imports cleanly but needs device-local values. Please enter:"
    PH_JSON="$(echo "$MISSING" | PRF_RNS_PH_IN="$PH_JSON" "${PY}" - <<'PY'
import json, os, sys
ph = json.loads(os.environ.get("PRF_RNS_PH_IN") or "{}")
for line in sys.stdin.read().splitlines():
    path = line.strip()
    if not path:
        continue
    sys.stderr.write(f"  {path}: ")
    sys.stderr.flush()
    val = sys.stdin.readline()
    if not val:
        # /dev/tty fallback when stdin already closed
        with open("/dev/tty") as t:
            val = t.readline()
    ph[path] = val.rstrip("\n")
print(json.dumps(ph))
PY
)"
  done
fi

echo "[6/6] enable + start"
systemctl enable predator-rns.service
systemctl restart predator-rns.service
sleep 1
systemctl --no-pager --lines=10 status predator-rns.service || true

echo
echo "Done. Tail logs with:  journalctl -u predator-rns -f"
