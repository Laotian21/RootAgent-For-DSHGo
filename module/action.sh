#!/system/bin/sh
# 点 SukiSU 模块卡片的「执行」时运行：确保令牌存在并展示，给 dsh-go 用。

MODDIR=$(dirname "$0")
DATA="${RA_DATA:-/data/adb/rootagent}"
SB="${RA_SB:-/sdcard/Download/RootAgent}"
PORT="${RA_PORT:-47831}"
TOKEN="$DATA/token"
PIDF="$DATA/rootagentd.pid"
LOCK="$DATA/watchdog.lock"

mkdir -p "$DATA" 2>/dev/null
[ -f "$MODDIR/lib.sh" ] && . "$MODDIR/lib.sh"

# 解压不保留权限位时 dshsb-exec 会变 0644，沙箱全部 Permission denied
for f in "$MODDIR"/bin/*; do [ -f "$f" ] && chmod 755 "$f" 2>/dev/null; done

# 旧看门狗会用自己缓存的参数重启守护进程，必须先停掉
stop_watchdog() {
  W=$(cat "$LOCK/pid" 2>/dev/null)
  [ -n "$W" ] && kill "$W" 2>/dev/null
  rm -rf "$LOCK" 2>/dev/null
}

if [ ! -s "$TOKEN" ]; then
  sh "$MODDIR/service.sh" >/dev/null 2>&1
fi
if [ ! -s "$TOKEN" ]; then
  gen_token > "$TOKEN"
  chmod 600 "$TOKEN" 2>/dev/null
fi

# 顺手识别一次目标 App 并记住包名（装完模块第一次点「执行」最有用）
P=$(ra_pkg 2>/dev/null)
[ -n "$P" ] && [ ! -f "$RA_CONF" ] && ra_set_pkg "$P"

# 守护进程没起就拉起来（刷完模块还没重启时用得上）
PID=$(cat "$PIDF" 2>/dev/null)
if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
  stop_watchdog
  sh "$MODDIR/service.sh" >/dev/null 2>&1
  sleep 1
  PID=$(cat "$PIDF" 2>/dev/null)
fi

# 进程活着但 /health 不 ok（忙死/挂起）就重启一次
health_ok() {
  [ -x /system/bin/curl ] || return 0
  /system/bin/curl -s -m 3 "http://127.0.0.1:$PORT/health" 2>/dev/null | grep -q '"ok":true'
}
if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null && ! health_ok; then
  echo "守护进程无响应（busy 或挂起），重启中…"
  stop_watchdog
  kill "$PID" 2>/dev/null
  sleep 1
  kill -9 "$PID" 2>/dev/null
  pkill -9 -f "$MODDIR/bin/rootagentd" 2>/dev/null
  sleep 1
  sh "$MODDIR/service.sh" >/dev/null 2>&1
  sleep 2
  PID=$(cat "$PIDF" 2>/dev/null)
fi

# 令牌镜像必须是 App uid 所有（否则引擎读不到，root 通道失效）
ra_mirror_token 2>/dev/null
rm -f "$SB/token.txt" 2>/dev/null

# 顺手把 proot 侧要装的三个文件放到共享存储，用户在 dsh-go 终端跑一趟 install.sh 即可
mkdir -p "$SB/proot" 2>/dev/null
for f in dshsb-client dsh-bwrap-root rootrun install-proot.sh; do
  [ -f "$MODDIR/bin/$f" ] || continue
  cp -f "$MODDIR/bin/$f" "$SB/proot/$f" 2>/dev/null && chmod 755 "$SB/proot/$f" 2>/dev/null
done

TOK=$(cat "$TOKEN" 2>/dev/null)
NOW=$(date '+%Y-%m-%d %H:%M:%S')
mkdir -p "$SB" 2>/dev/null
if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
  printf '状态: 运行中 (pid %s)\n端口: 127.0.0.1:%s\n时间: %s\n' "$PID" "$PORT" "$NOW" > "$SB/status.txt"
  STATE="运行中 (pid $PID)"
else
  printf '状态: 未运行（重启手机后再点一次执行）\n端口: 127.0.0.1:%s\n时间: %s\n' "$PORT" "$NOW" > "$SB/status.txt"
  STATE="未运行（重启手机后再点一次执行）"
fi

echo "Root Agent v1.3.0"
echo "状态: $STATE"
echo "端口: 127.0.0.1:$PORT"
echo "目标 App: ${P:-未识别（可在 $RA_CONF 里写 APP_PKG=<包名>）}"
echo "令牌: $TOK"
echo ""
echo "令牌等同 root 权限：不要截图外发。它也存在 App 私有目录："
echo "  <rootfs>/root/.dsh/rootagent-token（600，属主 App）"
echo ""
echo "proot 侧安装（只需要一次）:"
echo "  sh $SB/proot/install.sh"
echo ""
if [ -x /system/bin/curl ]; then
  echo "沙箱自检: $(/system/bin/curl -s -m 3 "http://127.0.0.1:$PORT/health" 2>/dev/null | sed -n 's/.*"sandbox":\(.*\)}}$/\1/p')"
fi
echo "自检: GET http://127.0.0.1:$PORT/health"
echo "执行: POST /exec  {\"cmd\":\"id -u\"}   (Authorization: Bearer <令牌>)"
