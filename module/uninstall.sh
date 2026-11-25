#!/system/bin/sh
# 卸载 Root Agent：先停看门狗与守护进程，再删令牌与日志

DATA="${RA_DATA:-/data/adb/rootagent}"
SB="${RA_SB:-/sdcard/Download/RootAgent}"
LOCK="$DATA/watchdog.lock"
PIDF="$DATA/rootagentd.pid"

WD=$(cat "$LOCK/pid" 2>/dev/null)
if [ -n "$WD" ]; then
  kill "$WD" 2>/dev/null
  sleep 0.5
  kill -9 "$WD" 2>/dev/null
fi

DP=$(cat "$PIDF" 2>/dev/null)
pkill -f "/data/adb/modules/rootagent/bin/rootagentd" 2>/dev/null
if [ -n "$DP" ]; then
  kill "$DP" 2>/dev/null
  sleep 0.5
  kill -9 "$DP" 2>/dev/null
fi

rm -rf "$DATA" 2>/dev/null

mkdir -p "$SB" 2>/dev/null
printf 'Root Agent 已卸载（%s）。此令牌已失效，重新安装会生成新令牌。\n' "$(date '+%Y-%m-%d %H:%M:%S')" > "$SB/UNINSTALLED.txt" 2>/dev/null
echo "Root Agent 已卸载：$DATA 已删除，令牌失效。"
