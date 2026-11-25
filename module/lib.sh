#!/system/bin/sh
# Root Agent 共用函数（由 service.sh / action.sh / customize.sh source）
# 目标 App（dsh-go）用包名定位：令牌镜像到它的私有目录，沙箱 socket 建在它的 rootfs 里。
# 包名来源：1) RA_PKG 环境变量（测试）2) $RA_DATA/app.conf 的 APP_PKG=（安装/执行时自动写）
#           3) 扫描 <data>/data/*/files/runtime/rootfs

RA_DATA="${RA_DATA:-/data/adb/rootagent}"
RA_CONF="$RA_DATA/app.conf"
RA_DATA_ROOT="${RA_DATA_ROOT:-/data}"

gen_token() {
  T=$(head -c 32 /dev/urandom | base64 2>/dev/null | tr -dc 'A-Za-z0-9' | head -c 32)
  if [ "$(printf '%s' "$T" | wc -c)" -ne 32 ]; then
    T=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' | head -c 32)
  fi
  printf '%s' "$T"
}

ra_pkg() {
  if [ -n "${RA_PKG:-}" ]; then printf '%s' "$RA_PKG"; return 0; fi
  if [ -f "$RA_CONF" ]; then
    APP_PKG=""
    . "$RA_CONF" 2>/dev/null
    if [ -n "$APP_PKG" ]; then printf '%s' "$APP_PKG"; return 0; fi
  fi
  for d in "$RA_DATA_ROOT"/data/*/files/runtime/rootfs "$RA_DATA_ROOT"/user/0/*/files/runtime/rootfs; do
    [ -d "$d" ] || continue
    p=${d#"$RA_DATA_ROOT"/data/}
    p=${p#"$RA_DATA_ROOT"/user/0/}
    p=${p%%/*}
    [ -n "$p" ] && { printf '%s' "$p"; return 0; }
  done
  return 1
}

ra_rootfs() {
  P=$(ra_pkg) || return 1
  for base in "$RA_DATA_ROOT/data" "$RA_DATA_ROOT/user/0"; do
    if [ -d "$base/$P/files/runtime/rootfs" ]; then
      printf '%s' "$base/$P/files/runtime/rootfs"; return 0
    fi
  done
  printf '%s' "$RA_DATA_ROOT/data/$P/files/runtime/rootfs"
}

ra_app_uid() {
  P=$(ra_pkg) || return 1
  for base in "$RA_DATA_ROOT/data" "$RA_DATA_ROOT/user/0"; do
    U=$(stat -c %u "$base/$P" 2>/dev/null)
    if [ -n "$U" ] && [ "$U" != 0 ]; then printf '%s' "$U"; return 0; fi
  done
  return 1
}

ra_set_pkg() {
  [ -n "$1" ] || return 1
  mkdir -p "$RA_DATA" 2>/dev/null
  printf 'APP_PKG=%s\n' "$1" > "$RA_CONF" 2>/dev/null && chmod 600 "$RA_CONF" 2>/dev/null
}

# 令牌镜像到 App 私有目录（属主 App uid、600）；拿不到 uid 就跳过，守护进程会兜底
ra_mirror_token() {
  R=$(ra_rootfs) || return 1
  U=$(ra_app_uid) || return 1
  [ -d "$R" ] || return 1
  T=$(cat "$RA_DATA/token" 2>/dev/null)
  [ -n "$T" ] || return 1
  mkdir -p "$R/root/.dsh" 2>/dev/null || return 1
  chown "$U" "$R/root/.dsh" 2>/dev/null
  printf '%s' "$T" > "$R/root/.dsh/rootagent-token" 2>/dev/null || return 1
  chmod 600 "$R/root/.dsh/rootagent-token" 2>/dev/null
  chown "$U" "$R/root/.dsh/rootagent-token" 2>/dev/null
}
