#!/system/bin/sh
# Root Agent v1.3.0 — 开机启动 rootagentd（KernelSU/SukiSU 模块）
# 重复执行不会起两份：先清残留进程，再 pidfile 检查 + 目录锁。

MODDIR=$(dirname "$0")
DATA="${RA_DATA:-/data/adb/rootagent}"
SB="${RA_SB:-/sdcard/Download/RootAgent}"
BIN="$MODDIR/bin/rootagentd"
PORT="${RA_PORT:-47831}"
TOKEN="$DATA/token"
LOG="$DATA/agent.log"
PIDF="$DATA/rootagentd.pid"
LOCK="$DATA/watchdog.lock"

[ -f "$BIN" ] || { echo "rootagentd 缺失: $BIN"; exit 1; }
chmod 755 "$BIN" 2>/dev/null
# 部分管理器解压时不保留权限位（沙箱命令会全部 Permission denied），每次启动自愈一遍
for f in "$MODDIR"/bin/*; do [ -f "$f" ] && chmod 755 "$f" 2>/dev/null; done
for f in "$MODDIR"/*.sh; do [ -f "$f" ] && chmod 755 "$f" 2>/dev/null; done

mkdir -p "$DATA" 2>/dev/null
chmod 700 "$DATA" 2>/dev/null
[ -f "$MODDIR/lib.sh" ] && . "$MODDIR/lib.sh"

# 0) 清掉残留守护进程（含还占着端口的旧子进程）
if command -v pkill >/dev/null 2>&1; then
  pkill -f "$BIN" 2>/dev/null
  sleep 1
  pkill -9 -f "$BIN" 2>/dev/null
fi

# 1) 令牌：每台设备唯一，存在就不再变
[ -s "$TOKEN" ] || gen_token > "$TOKEN"
chmod 600 "$TOKEN" 2>/dev/null

# 2) 镜像令牌到 App 私有目录。开机早期 CE 未解锁时拿不到 uid 就跳过
#    （不要 chown 0 / chmod 600），守护进程会在目录就绪后自己推导属主并兜底。
P=$(ra_pkg 2>/dev/null)
[ -n "$P" ] && [ ! -f "$RA_CONF" ] && ra_set_pkg "$P"
ra_mirror_token 2>/dev/null
mkdir -p "$SB" 2>/dev/null
printf '令牌只在 App 私有目录：<rootfs>/root/.dsh/rootagent-token（沙箱内不可见）\n' > "$SB/README.txt" 2>/dev/null

# 3) 沙箱参数：rootfs 还没出现也照传，守护进程会每 5 秒重试 bind
sock_args() {
  R=$(ra_rootfs 2>/dev/null) || return 0
  printf '%s' "--sandbox-sock $R/root/.dsh/rootagent-sandbox.sock --rootfs $R"
  U=$(ra_app_uid 2>/dev/null)
  [ -n "$U" ] && printf ' --sock-uid %s' "$U"
  return 0
}
ARGS=$(sock_args)

# 4) 已在跑就不再起
if [ -f "$PIDF" ]; then
  OLD=$(cat "$PIDF" 2>/dev/null)
  [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null && exit 0
fi

# 4.5) 清旧看门狗：否则它会用自己缓存的旧参数抢先拉起守护进程
if [ -f "$LOCK/pid" ]; then
  W=$(cat "$LOCK/pid" 2>/dev/null)
  [ -n "$W" ] && kill "$W" 2>/dev/null
fi
rm -rf "$LOCK" 2>/dev/null
mkdir "$LOCK" 2>/dev/null || exit 0

# 5) 看门狗：退出 2 秒后拉起；/health 连续 3 次不 ok（忙死/挂起）也重启。
#    若 rootfs 是开机后才出现，下一轮补上沙箱参数并重启一次守护进程。
health_ok() {
  [ -x /system/bin/curl ] || return 0
  /system/bin/curl -s -m 3 "http://127.0.0.1:$PORT/health" 2>/dev/null | grep -q '"ok":true'
}
(
  CUR_ARGS="$ARGS"
  while true; do
    if [ -z "$CUR_ARGS" ]; then
      NEW=$(sock_args)
      if [ -n "$NEW" ]; then
        echo "$(date '+%Y-%m-%dT%H:%M:%SZ') watchdog: rootfs 就绪，补沙箱参数重启 rootagentd" >> "$LOG"
        CUR_ARGS="$NEW"
        [ -n "$CHILD" ] && kill -9 "$CHILD" 2>/dev/null && sleep 2
      fi
    fi
    if [ -x /system/bin/curl ] && health_ok; then
      sleep 10
      continue
    fi
    "$BIN" --port "$PORT" --token-file "$TOKEN" --log "$LOG" --pidfile "$PIDF" $CUR_ARGS 2>>"$LOG" &
    CHILD=$!
    FAIL=0
    while kill -0 "$CHILD" 2>/dev/null; do
      sleep 10
      if health_ok; then
        FAIL=0
      else
        FAIL=$((FAIL + 1))
        if [ "$FAIL" -ge 3 ]; then
          echo "$(date '+%Y-%m-%dT%H:%M:%SZ') watchdog: health 连续 3 次异常，重启 rootagentd" >> "$LOG"
          kill -9 "$CHILD" 2>/dev/null
          FAIL=0
        fi
      fi
    done
    sleep 2
  done
) >/dev/null 2>&1 &
echo $! > "$LOCK/pid"

exit 0
