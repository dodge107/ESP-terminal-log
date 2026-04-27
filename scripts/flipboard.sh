#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────
#  flipboard.sh  –  CLI controller for the FlipBoard ESP32
#  https://github.com/dodge107/ESP-terminal-log
# ─────────────────────────────────────────────────────────────
set -euo pipefail

# ── Config ────────────────────────────────────────────────────
CONFIG_FILE="$HOME/.flipboard_config"

# ── Colours ───────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

# ── Helpers ───────────────────────────────────────────────────
log()     { echo -e "${CYAN}▶${RESET}  $*"; }
ok()      { echo -e "${GREEN}✔${RESET}  $*"; }
warn()    { echo -e "${YELLOW}⚠${RESET}  $*"; }
err()     { echo -e "${RED}✖${RESET}  $*" >&2; }
section() { echo -e "\n${BOLD}── $* ──${RESET}"; }

# ── Load / save config ────────────────────────────────────────
load_config() {
  if [[ -f "$CONFIG_FILE" ]]; then
    # shellcheck source=/dev/null
    source "$CONFIG_FILE"
  fi
  BOARD_IP="${BOARD_IP:-}"
  API_KEY="${API_KEY:-}"
}

save_config() {
  cat > "$CONFIG_FILE" <<EOF
BOARD_IP="$BOARD_IP"
API_KEY="$API_KEY"
EOF
  chmod 600 "$CONFIG_FILE"
  ok "Config saved to $CONFIG_FILE"
}

require_config() {
  if [[ -z "$BOARD_IP" || -z "$API_KEY" ]]; then
    err "Board IP and API key are not set. Run:  $0 configure"
    exit 1
  fi
}

# ── HTTP helpers ──────────────────────────────────────────────
API() {
  # API <METHOD> <PATH> [body]
  local method="$1" path="$2" body="${3:-}"
  local url="http://${BOARD_IP}${path}"
  local args=(-s -w "\n%{http_code}" -X "$method" -H "X-Api-Key: $API_KEY")

  if [[ -n "$body" ]]; then
    args+=(-H "Content-Type: text/plain" --data-raw "$body")
  fi

  local response http_code
  response=$(curl "${args[@]}" "$url" 2>&1) || {
    err "curl failed – is the board reachable at $BOARD_IP?"
    exit 1
  }

  http_code="${response##*$'\n'}"
  local body_out="${response%$'\n'*}"

  case "$http_code" in
    200|201|204) echo "$body_out" ;;
    401) err "Unauthorised (401) – check your API key"; exit 1 ;;
    413) err "Body too large (413) – max 512 bytes per request"; exit 1 ;;
    429) err "Rate limited (429) – slow down (max 10 req/s)"; exit 1 ;;
    *)   err "HTTP $http_code"; [[ -n "$body_out" ]] && echo "$body_out" >&2; exit 1 ;;
  esac
}

# Sanitise text: uppercase, strip unsupported chars, truncate to 21
sanitise() {
  echo "$1" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9 \-:/.!' | cut -c1-21
}

# ── Commands ──────────────────────────────────────────────────

cmd_configure() {
  section "Configure FlipBoard connection"
  read -rp "  Board IP address  [${BOARD_IP:-none}]: " input
  [[ -n "$input" ]] && BOARD_IP="$input"

  read -rp "  API key           [${API_KEY:+****}]: " input
  [[ -n "$input" ]] && API_KEY="$input"

  if [[ -z "$BOARD_IP" || -z "$API_KEY" ]]; then
    err "Both IP and API key are required."
    exit 1
  fi
  save_config
}

cmd_status() {
  require_config
  section "Board status"
  local json
  json=$(API GET /status)
  # Pretty-print if python3 is available, else raw
  if command -v python3 &>/dev/null; then
    echo "$json" | python3 -m json.tool 2>/dev/null || echo "$json"
  else
    echo "$json"
  fi
}

cmd_set_row() {
  require_config
  local row="${1:-}"
  local text="${2:-}"

  if [[ -z "$row" ]]; then
    read -rp "  Row number (0-5): " row
  fi
  if [[ ! "$row" =~ ^[0-5]$ ]]; then
    err "Row must be 0–5"; exit 1
  fi
  if [[ -z "$text" ]]; then
    read -rp "  Text (max 21 chars): " text
  fi

  text=$(sanitise "$text")
  log "Setting row $row → \"$text\""
  API POST "/row/$row" "$text" > /dev/null
  ok "Row $row updated"
}

cmd_set_all() {
  require_config
  section "Set all 6 rows"
  declare -a rows
  for i in {0..5}; do
    read -rp "  Row $i (max 21 chars): " input
    rows[$i]=$(sanitise "$input")
  done

  local body
  body=$(printf '%s\n' "${rows[@]}")
  # Remove trailing newline
  body="${body%$'\n'}"

  log "Sending all rows…"
  API POST /rows "$body" > /dev/null
  ok "All rows updated"
}

cmd_clear_row() {
  require_config
  local row="${1:-}"
  if [[ -z "$row" ]]; then
    read -rp "  Row number to clear (0-5): " row
  fi
  if [[ ! "$row" =~ ^[0-5]$ ]]; then
    err "Row must be 0–5"; exit 1
  fi
  log "Clearing row $row…"
  API DELETE "/row/$row/clear" > /dev/null
  ok "Row $row cleared"
}

cmd_clear_all() {
  require_config
  warn "This will blank all 6 rows."
  read -rp "  Confirm? [y/N] " confirm
  [[ "$(echo "$confirm" | tr '[:upper:]' '[:lower:]')" != "y" ]] && { log "Aborted."; return; }
  local body
  body=$(printf ' \n \n \n \n \n ')
  API POST /rows "$body" > /dev/null
  ok "All rows cleared"
}

cmd_preset() {
  require_config
  section "Load a preset"
  echo "  1) Flight board"
  echo "  2) Stock ticker"
  echo "  3) Custom message (3 rows)"
  read -rp "  Choice [1-3]: " choice

  case "$choice" in
    1)
      local body="FL 101  LONDON"$'\n'"FL 202  NEW YORK"$'\n'"FL 303  PARIS"$'\n'"FL 404  TOKYO"$'\n'"FL 505  SYDNEY"$'\n'"FL 606  DUBAI"
      ;;
    2)
      local body="AAPL  172.45 +1.2%"$'\n'"TSLA  248.10 -0.8%"$'\n'"NVDA  875.22 +3.4%"$'\n'"BTC  67420 +2.1%"$'\n'"ETH   3510 +1.7%"$'\n'"SPY  523.80 +0.5%"
      ;;
    3)
      read -rp "  Line 1: " l1; l1=$(sanitise "$l1")
      read -rp "  Line 2: " l2; l2=$(sanitise "$l2")
      read -rp "  Line 3: " l3; l3=$(sanitise "$l3")
      local body="$l1"$'\n'"$l2"$'\n'"$l3"$'\n'$'\n'$'\n'
      ;;
    *)
      err "Invalid choice"; exit 1 ;;
  esac

  log "Sending preset…"
  API POST /rows "$body" > /dev/null
  ok "Preset loaded"
}

cmd_set_timeout() {
  require_config
  local mins="${1:-}"
  if [[ -z "$mins" ]]; then
    read -rp "  Display off timeout in minutes (0 = never): " mins
  fi
  if [[ ! "$mins" =~ ^[0-9]+$ ]] || (( mins > 1440 )); then
    err "Minutes must be a number between 0 and 1440"; exit 1
  fi
  log "Setting display off timeout to $mins minute(s)…"
  API POST /display/timeout "$mins" > /dev/null
  if [[ "$mins" == "0" ]]; then
    ok "Display will stay on indefinitely"
  else
    ok "Display will power off after $mins minute(s) of inactivity"
  fi
}

cmd_wifi_reset() {
  require_config
  warn "This will erase WiFi credentials and reboot the board."
  read -rp "  Confirm? [y/N] " confirm
  [[ "$(echo "$confirm" | tr '[:upper:]' '[:lower:]')" != "y" ]] && { log "Aborted."; return; }
  log "Sending wifi reset…"
  API POST /wifi/reset > /dev/null || true
  ok "Board is rebooting – reconnect to its captive portal to reconfigure WiFi."
}

cmd_interactive() {
  while true; do
    echo ""
    echo -e "${BOLD}FlipBoard Controller${RESET}  ${DIM}(${BOARD_IP:-not configured})${RESET}"
    echo "  1) Set one row"
    echo "  2) Set all rows"
    echo "  3) Clear one row"
    echo "  4) Clear all rows"
    echo "  5) Load a preset"
    echo "  6) Get board status"
    echo "  7) Set display off timeout"
    echo "  8) Configure (IP / API key)"
    echo "  9) Reset WiFi"
    echo "  q) Quit"
    echo ""
    read -rp "Choice: " choice
    case "$choice" in
      1) cmd_set_row ;;
      2) cmd_set_all ;;
      3) cmd_clear_row ;;
      4) cmd_clear_all ;;
      5) cmd_preset ;;
      6) cmd_status ;;
      7) cmd_set_timeout ;;
      8) cmd_configure ;;
      9) cmd_wifi_reset ;;
      q|Q) echo "Bye!"; exit 0 ;;
      *) warn "Unknown option" ;;
    esac
  done
}

# ── Usage ─────────────────────────────────────────────────────
usage() {
  local s; s=$(basename "$0")
  echo -e ""
  echo -e "${BOLD}Usage:${RESET} $s [command] [args]"
  echo -e ""
  echo -e "${BOLD}Commands:${RESET}"
  echo -e "  configure             Save board IP and API key"
  echo -e "  status                Show board status JSON"
  echo -e "  row <0-5> [text]      Set a single row"
  echo -e "  rows                  Set all 6 rows interactively"
  echo -e "  clear <0-5>           Clear a single row"
  echo -e "  clear-all             Blank every row"
  echo -e "  preset                Load a built-in preset"
  echo -e "  timeout [minutes]     Set display off timeout (0 = never)"
  echo -e "  wifi-reset            Erase WiFi credentials and reboot"
  echo -e "  (no command)          Launch interactive menu"
  echo -e ""
  echo -e "${BOLD}Examples:${RESET}"
  echo -e "  $s configure"
  echo -e "  $s row 0 \"GATE CHANGE B12\""
  echo -e "  $s status"
  echo -e "  $s clear 3"
  echo -e ""
  echo -e "${BOLD}Config file:${RESET} $CONFIG_FILE"
  echo -e "  Set BOARD_IP and API_KEY there, or export them as env vars"
  echo -e "  before running this script."
  echo -e ""
}

# ── Entry point ───────────────────────────────────────────────
load_config

# Allow env-var overrides
BOARD_IP="${BOARD_IP_OVERRIDE:-$BOARD_IP}"
API_KEY="${API_KEY_OVERRIDE:-$API_KEY}"

case "${1:-}" in
  configure)   cmd_configure ;;
  status)      cmd_status ;;
  row)         cmd_set_row "${2:-}" "${3:-}" ;;
  rows)        cmd_set_all ;;
  clear)       cmd_clear_row "${2:-}" ;;
  clear-all)   cmd_clear_all ;;
  preset)      cmd_preset ;;
  timeout)     cmd_set_timeout "${2:-}" ;;
  wifi-reset)  cmd_wifi_reset ;;
  -h|--help)   usage ;;
  "")          cmd_interactive ;;
  *)           err "Unknown command: $1"; usage; exit 1 ;;
esac
