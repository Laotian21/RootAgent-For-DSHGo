#!/bin/sh
# 在 dsh-go 的 proot 终端里执行：把沙箱 client / shim / rootrun 装进 /usr/local/bin
# 用法：sh /sdcard/Download/RootAgent/proot/install.sh [目标目录]
SRC=$(cd "$(dirname "$0")" && pwd)
DST=${1:-/usr/local/bin}

mkdir -p "$DST" || exit 1
for f in dshsb-client dsh-bwrap-root rootrun; do
  if [ ! -f "$SRC/$f" ]; then
    echo "缺 $SRC/$f（先在 SukiSU 模块页点一次「执行」同步文件）" >&2
    exit 1
  fi
  cp -f "$SRC/$f" "$DST/$f" || exit 1
  chmod 755 "$DST/$f" || exit 1
  echo "  ✓ $DST/$f"
done

echo
echo "装好了。验证："
echo "  dsh-bwrap-root --help 2>&1 | head -2"
echo "  rootrun 'id -u'          # 应输出 0"
