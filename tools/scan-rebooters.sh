#!/usr/bin/env bash
# Scan local LAN for Rebooter devices on port 80
set -euo pipefail

# Detect default interface, IP, and CIDR
iface=$(ip route show default | awk '/default/{print $5; exit}')
cidr=$(ip -4 addr show "$iface" | awk '/inet /{print $2; exit}')

if [[ -z "$cidr" ]]; then
  echo "Could not determine local subnet." >&2
  exit 1
fi

local_ip=${cidr%/*}
mask_bits=${cidr#*/}
echo "Interface: $iface"
echo "Local IP:  $local_ip/$mask_bits"
echo ""
echo "Scanning $cidr for port 80..."
echo ""

# Fast SYN scan (falls back to connect scan without root)
hosts=$(nmap -n -p 80 --open -oG - "$cidr" 2>/dev/null \
  | awk '/80\/open/{print $2}')

if [[ -z "$hosts" ]]; then
  echo "No hosts with port 80 open."
  exit 0
fi

printf "%-16s  %-8s  %s\n" "IP" "MATCH" "DETAILS"
printf "%-16s  %-8s  %s\n" "---" "-----" "-------"

for ip in $hosts; do
  body=$(curl -s --connect-timeout 2 --max-time 3 "http://$ip/" 2>/dev/null || true)

  title=""
  if [[ -n "$body" ]]; then
    title=$(echo "$body" | grep -oiP '(?<=<title>)[^<]+' | head -1)
  fi

  if echo "$body" | grep -qi "rebooter"; then
    tag="YES"
    h1=$(echo "$body" | grep -oiP '(?<=<h1>)[^<]+' | head -1)
    detail="title=\"$title\" h1=\"$h1\""
  else
    tag="no"
    detail="title=\"$title\""
  fi

  printf "%-16s  %-8s  %s\n" "$ip" "$tag" "$detail"
done
