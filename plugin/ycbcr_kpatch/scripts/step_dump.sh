#!/usr/bin/env bash
# One payload per window. NEVER echo to :9090.
# Liveness via FTP :2121 only. After nc, long wait before declaring OK.
set -euo pipefail

PS4="${PS4_IP:-192.168.0.175}"
PS4SDK="${PS4SDK:-/tmp/ps4-payload-sdk}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/../bin/ycbcr_kdump_900.bin"
OUT="${OUT:-$ROOT/../../pkg/assets/misc/kernel_900.bin}"
LOG="${LOG:-/tmp/ycbcr_kdump_step.log}"

OFF="${DUMP_OFF:-0x100000}"
END="${DUMP_END:-0xd00000}"
LEN="${DUMP_LEN:-0x100000}"  # 1 MiB

# Known holes (trap 12): start:end (end exclusive)
HOLES="${DUMP_HOLES:-0x90000:0x100000 0x254000:0x260000}"

export PS4SDK

ftp_size() {
  curl -sS --connect-timeout 3 --max-time 15 \
    -I "ftp://$PS4:2121/data/moonlight/kernel_900.bin" 2>/dev/null \
    | awk -F': ' 'tolower($1)=="content-length"{gsub(/\r/,"",$2); print $2; exit}'
}

ftp_up() {
  timeout 3 bash -c "echo >/dev/tcp/$PS4/2121" 2>/dev/null
}

wait_ftp() {
  echo "Waiting for FTP $PS4:2121 ..." | tee -a "$LOG"
  while ! ftp_up; do sleep 5; done
  echo "FTP OK $(date)" | tee -a "$LOG"
  sleep 5
}

# If OFF falls in a hole, jump to the hole's end
skip_holes() {
  local o=$1 h s e
  for h in $HOLES; do
    s=${h%%:*}; e=${h##*:}
    if [ $((o)) -ge $((s)) ] && [ $((o)) -lt $((e)) ]; then
      echo "$e"
      return
    fi
  done
  echo "$o"
}

# Clip WLEN so we do not enter the next hole
clip_len() {
  local o=$1 L=$2 h s e
  for h in $HOLES; do
    s=${h%%:*}; e=${h##*:}
    if [ $((o)) -lt $((s)) ] && [ $((o + L)) -gt $((s)) ]; then
      L=$((s - o))
    fi
  done
  echo "$(printf '0x%x' "$L")"
}

: > "$LOG"
echo "=== step dump $(date) off=$OFF end=$END len=$LEN holes=[$HOLES] ===" | tee -a "$LOG"
wait_ftp

while [ $((OFF)) -lt $((END)) ]; do
  OFF=$(skip_holes "$OFF")
  if [ $((OFF)) -ge $((END)) ]; then
    break
  fi

  WLEN=$LEN
  if [ $((OFF + WLEN)) -gt $((END)) ]; then
    WLEN=$(printf '0x%x' $((END - OFF)))
  fi
  WLEN=$(clip_len "$OFF" "$WLEN")
  if [ $((WLEN)) -le 0 ]; then
    OFF=$(skip_holes "$(printf '0x%x' $((OFF + 0x1000)))")
    continue
  fi

  if ! ftp_up; then
    echo "STOP: FTP down before off=$OFF" | tee -a "$LOG"
    exit 2
  fi

  echo "--- off=$OFF len=$WLEN ---" | tee -a "$LOG"
  make -C "$ROOT" dump DUMP_OFF="$OFF" DUMP_LEN="$WLEN" >>"$LOG" 2>&1

  set +e
  nc -w 120 "$PS4" 9090 < "$BIN"
  NC=$?
  set -e
  if [ "$NC" -ne 0 ]; then
    echo "STOP: nc exit=$NC off=$OFF" | tee -a "$LOG"
    exit 3
  fi

  # Give time for a trap to take the network down before declaring OK
  sleep 8

  if ! ftp_up; then
    echo "STOP: FTP down after off=$OFF (panic likely)" | tee -a "$LOG"
    exit 2
  fi

  SZ=$(ftp_size || echo "?")
  echo "OK off=$OFF len=$WLEN remote_size=$SZ" | tee -a "$LOG"
  OFF=$(printf '0x%x' $((OFF + WLEN)))
done

echo "=== FTP pull ===" | tee -a "$LOG"
curl -sS --connect-timeout 5 --max-time 600 -o "$OUT" \
  "ftp://$PS4:2121/data/moonlight/kernel_900.bin"
ls -la "$OUT" | tee -a "$LOG"
python3 "$ROOT/scripts/scan_kernel_dump.py" "$OUT" 2>&1 | tee -a "$LOG" | tail -40
echo "=== done $(date) ===" | tee -a "$LOG"
