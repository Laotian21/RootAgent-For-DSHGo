#!/bin/sh
# 构建：编译三个二进制到 module/bin，复制 proot 侧脚本，再打包成可刷入的 zip
# 用法：sh src/build.sh        CC / CFLAGS 可覆盖；默认静态链接，方便在恢复环境里跑
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -static -s -Wall -Wextra}

mkdir -p "$ROOT/module/bin" "$ROOT/dist"

echo "[1/4] 编译 rootagentd"
$CC $CFLAGS -o "$ROOT/module/bin/rootagentd" "$ROOT/src/rootagentd.c"

echo "[2/4] 编译 dshsb-exec / dshsb-client"
$CC $CFLAGS -o "$ROOT/module/bin/dshsb-exec" "$ROOT/src/dshsb-exec.c"
$CC $CFLAGS -o "$ROOT/module/bin/dshsb-client" "$ROOT/src/dshsb-client.c"

echo "[3/4] 复制 proot 侧脚本"
cp -f "$ROOT/src/dsh-bwrap-root" "$ROOT/module/bin/dsh-bwrap-root"
cp -f "$ROOT/src/rootrun" "$ROOT/module/bin/rootrun"
chmod 755 "$ROOT/module/bin/"*

VER=$(sed -n 's/^version=//p' "$ROOT/module/module.prop")
ZIP="$ROOT/dist/RootAgent-$VER.zip"
if ! command -v zip >/dev/null 2>&1; then
  echo "缺 zip 命令：apt-get install -y zip" >&2
  exit 1
fi
rm -f "$ZIP"
echo "[4/4] 打包 $ZIP"
(cd "$ROOT/module" && zip -qr "$ZIP" . -x "*.DS_Store")
ls -l "$ZIP"
echo "完成。刷入前可解压检查 bin/ 里的文件权限。"
