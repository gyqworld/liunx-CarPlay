#!/usr/bin/env bash
# SoftAP for wireless CarPlay with Apple Device IE (CarPlay feature bit).
# Without that beacon IE, iPhone treats capacity as missing and encodes ~1 Mbps.
#
# Usage:
#   sudo carplay-hotspot.sh start --iface wlo1 --ssid NAME --pass PASS [--channel 6] [--bt-mac AA:BB:...]
#   sudo carplay-hotspot.sh stop
#   carplay-hotspot.sh status
set -euo pipefail

STATE_DIR="${CARPLAY_HOTSPOT_STATE:-/run/carplay-hotspot}"
CONF_DIR="${CARPLAY_HOTSPOT_CONF:-/etc/carplay-cpp}"
HOSTAPD_CONF="$STATE_DIR/hostapd.conf"
DNSMASQ_CONF="$STATE_DIR/dnsmasq.conf"
HOSTAPD_PID="$STATE_DIR/hostapd.pid"
DNSMASQ_PID="$STATE_DIR/dnsmasq.pid"
IFACE_FILE="$STATE_DIR/iface"
VIRTUAL_FILE="$STATE_DIR/virtual"
AP_ADDR="${CARPLAY_HOTSPOT_ADDR:-192.168.50.1}"
DHCP_START="${CARPLAY_HOTSPOT_DHCP_START:-192.168.50.10}"
DHCP_END="${CARPLAY_HOTSPOT_DHCP_END:-192.168.50.50}"

log() { echo "[carplay-hotspot] $*"; }
die() { log "ERROR: $*"; exit 1; }

need_root() {
  if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    die "start/stop require root (sudo)"
  fi
}

normalize_mac() {
  echo "$1" | tr '[:upper:]' '[:lower:]' | tr -d ':-' | tr -d ' '
}

# Apple vendor IE (Element ID 0xDD, OUI 00:A0:40) + Flags (CarPlay|2.4G|5G claim)
# + optional Bluetooth MAC sub-element + Interworking IE (0x6B).
build_vendor_elements() {
  local bt_hex="$1"
  local flags="00020023" # Flags ID=00 len=02 value=00 23 (CarPlay+2.4+5G bits)
  local body="00a04000${flags}"
  if [[ ${#bt_hex} -eq 12 ]]; then
    body="${body}0606${bt_hex}"
  fi
  local len=$(( ${#body} / 2 ))
  local len_hex
  len_hex=$(printf '%02x' "$len")
  # Interworking: private network with Internet bit set (helps some iOS versions)
  echo "dd${len_hex}${body}6b0110"
}

write_hostapd() {
  local iface="$1" ssid="$2" pass="$3" channel="$4" vendor="$5" hw_mode="$6" set_country="$7"
  local country=""
  if [[ "$set_country" == "1" ]]; then
    country=$'country_code=CN\nieee80211d=1'
  fi
  cat >"$HOSTAPD_CONF" <<EOF
interface=$iface
driver=nl80211
ssid=$ssid
$country
hw_mode=$hw_mode
channel=$channel
ieee80211n=1
ht_capab=[SHORT-GI-20][HT20]
wmm_enabled=1
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=$pass
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
# Apple Device IE: without it iPhone encodes at the ~1 Mbps floor.
vendor_elements=$vendor
EOF
}

write_dnsmasq() {
  local iface="$1"
  cat >"$DNSMASQ_CONF" <<EOF
interface=$iface
bind-interfaces
dhcp-range=$DHCP_START,$DHCP_END,255.255.255.0,12h
dhcp-option=3,$AP_ADDR
dhcp-option=6,$AP_ADDR
no-resolv
log-queries
EOF
}

cmd_stop() {
  need_root
  mkdir -p "$STATE_DIR"
  if [[ -f "$HOSTAPD_PID" ]]; then
    kill "$(cat "$HOSTAPD_PID")" 2>/dev/null || true
    rm -f "$HOSTAPD_PID"
  fi
  if [[ -f "$DNSMASQ_PID" ]]; then
    kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true
    rm -f "$DNSMASQ_PID"
  fi
  pkill -f "hostapd $HOSTAPD_CONF" 2>/dev/null || true
  pkill -f "dnsmasq --conf-file=$DNSMASQ_CONF" 2>/dev/null || true
  local iface="" virtual=0
  [[ -f "$IFACE_FILE" ]] && iface=$(cat "$IFACE_FILE")
  [[ -f "$VIRTUAL_FILE" ]] && virtual=1
  if [[ -n "$iface" ]]; then
    ip addr flush dev "$iface" 2>/dev/null || true
    if [[ "$virtual" == "1" ]]; then
      iw dev "$iface" del 2>/dev/null || true
    else
      ip link set "$iface" down 2>/dev/null || true
      nmcli device set "$iface" managed yes 2>/dev/null || true
    fi
  fi
  rm -f "$HOSTAPD_CONF" "$DNSMASQ_CONF" "$IFACE_FILE" "$VIRTUAL_FILE"
  log "stopped"
}

cmd_status() {
  if [[ -f "$HOSTAPD_PID" ]] && kill -0 "$(cat "$HOSTAPD_PID")" 2>/dev/null; then
    local iface=""
    [[ -f "$IFACE_FILE" ]] && iface=$(cat "$IFACE_FILE")
    local ap_mac="unknown"
    [[ -n "$iface" && -r "/sys/class/net/$iface/address" ]] && ap_mac=$(cat "/sys/class/net/$iface/address")
    local ap_ch=""
    [[ -n "$iface" ]] && ap_ch=$(iw dev "$iface" info 2>/dev/null | awk '/channel/{print $2; exit}')
    echo "running iface=${iface:-?} addr=$AP_ADDR channel=${ap_ch:-?} mac=$ap_mac"
    exit 0
  fi
  echo "stopped"
  exit 1
}

cmd_start() {
  need_root
  local iface="wlo1" ssid="专业拧瓶盖员" pass="" channel=6 bt_mac="" alongside=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --iface) iface="$2"; shift 2 ;;
      --ssid) ssid="$2"; shift 2 ;;
      --pass|--password) pass="$2"; shift 2 ;;
      --channel) channel="$2"; shift 2 ;;
      --bt-mac) bt_mac="$2"; shift 2 ;;
      --alongside) alongside=1; shift ;;
      *) die "unknown arg: $1" ;;
    esac
  done
  [[ -n "$pass" ]] || die "password required (--pass)"
  [[ ${#pass} -ge 8 ]] || die "password must be >= 8 chars"
  command -v hostapd >/dev/null || die "hostapd not installed"
  command -v dnsmasq >/dev/null || die "dnsmasq not installed"
  command -v nmcli >/dev/null || die "nmcli not installed"

  local ap_iface="$iface" hw_mode="g" set_country=1
  if [[ "$alongside" == "1" ]]; then
    # STA+AP must share one channel. 5 GHz here is IR-CONCURRENT: AP is allowed
    # only while the station is already associated on that channel.
    local sta_ch
    sta_ch=$(iw dev "$iface" info 2>/dev/null | awk '/channel/{print $2; exit}')
    [[ -n "${sta_ch:-}" && "$sta_ch" != "0" ]] || die "当前 Wi-Fi 未连接，无法在同一信道加 CarPlay 热点"
    channel="$sta_ch"
    ap_iface="carplay0"
    local phy
    phy=$(iw dev "$iface" info 2>/dev/null | awk '/wiphy/{print "phy"$2; exit}')
    [[ -n "$phy" ]] || phy=phy0
    if iw phy "$phy" info 2>/dev/null | grep -q "\[$channel\].*no IR"; then
      die "当前 Wi-Fi 在信道 ${channel}，这张网卡不允许在该信道开热点，启动接收已不依赖这一步。要提高码率请改用「本机移动热点」（2.4GHz，会暂时离开路由器）。"
    fi
    if [[ "$channel" -gt 14 ]]; then
      hw_mode="a"
      set_country=0
    fi
  else
    case "$channel" in
      1|6|11) ;;
      *)
        log "channel $channel not ideal for CarPlay 2.4GHz; forcing 6"
        channel=6
        ;;
    esac
  fi

  cmd_stop >/dev/null 2>&1 || true
  mkdir -p "$STATE_DIR" "$CONF_DIR"

  if [[ "$alongside" == "1" ]]; then
    iw dev "$iface" interface add "$ap_iface" type __ap || die "无法在现有 Wi-Fi 旁增加热点（需同一信道）"
    echo 1 >"$VIRTUAL_FILE"
    nmcli device set "$ap_iface" managed no 2>/dev/null || true
    sleep 0.3
  else
    nmcli device disconnect "$iface" 2>/dev/null || true
    nmcli device set "$iface" managed no
    sleep 1
  fi

  local bt_hex
  bt_hex=$(normalize_mac "$bt_mac")
  local vendor
  vendor=$(build_vendor_elements "$bt_hex")
  write_hostapd "$ap_iface" "$ssid" "$pass" "$channel" "$vendor" "$hw_mode" "$set_country"
  write_dnsmasq "$ap_iface"

  ip link set "$ap_iface" up
  ip addr flush dev "$ap_iface"
  ip addr add "$AP_ADDR/24" dev "$ap_iface"
  echo "$ap_iface" >"$IFACE_FILE"

  if ! hostapd -B -P "$HOSTAPD_PID" "$HOSTAPD_CONF"; then
    cmd_stop >/dev/null 2>&1 || true
    die "hostapd failed on ${hw_mode} channel ${channel} (5GHz AP only works beside an already-associated station on that channel)"
  fi
  sleep 0.4
  if ! iw dev "$ap_iface" info 2>/dev/null | grep -q "type AP"; then
    die "AP did not stay up on channel ${channel}"
  fi
  dnsmasq --conf-file="$DNSMASQ_CONF" --pid-file="$DNSMASQ_PID" || die "dnsmasq failed"

  # Optional: allow phone to reach LAN/WAN through this box.
  sysctl -w net.ipv4.ip_forward=1 >/dev/null
  # Prefer NAT via the previous default route interface if any.
  local wan
  wan=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
  if [[ -n "${wan:-}" && "$wan" != "$ap_iface" ]]; then
    iptables -t nat -C POSTROUTING -o "$wan" -j MASQUERADE 2>/dev/null || \
      iptables -t nat -A POSTROUTING -o "$wan" -j MASQUERADE
  fi

  local ap_mac="unknown"
  [[ -r "/sys/class/net/$ap_iface/address" ]] && ap_mac=$(cat "/sys/class/net/$ap_iface/address")
  log "started ssid=$ssid channel=$channel iface=$ap_iface hw=$hw_mode alongside=$alongside addr=$AP_ADDR mac=$ap_mac"
  log "Apple IE vendor_elements=$vendor"
  log "Reconnect iPhone to this SoftAP (not the home router) then start CarPlay"
}

case "${1:-}" in
  start) shift; cmd_start "$@" ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  *)
    echo "Usage: $0 start --iface IF --ssid SSID --pass PASS [--channel 6] [--bt-mac MAC]"
    echo "       $0 stop | status"
    exit 2
    ;;
esac
