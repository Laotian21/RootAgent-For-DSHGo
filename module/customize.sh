#!/system/bin/sh
# 安装时执行（KernelSU/SukiSU 在刷入后调用）
ui_print "Root Agent v1.3.0 已安装"
ui_print "重启手机后生效；重启完在模块卡片点『执行』即可看到本机令牌。"

# 有些管理器解压不保留权限位，这里补齐（service.sh 每次启动也会自愈）
[ -d "$MODPATH/bin" ] && chmod 755 "$MODPATH/bin/"* 2>/dev/null
for f in "$MODPATH"/*.sh; do [ -f "$f" ] && chmod 755 "$f" 2>/dev/null; done

# 顺手记下目标 App 包名（安装时 /data 可读）；没识别到也不影响，点「执行」时会再试
mkdir -p /data/adb/rootagent 2>/dev/null
if [ -f "$MODPATH/lib.sh" ]; then
  . "$MODPATH/lib.sh"
  PKG=$(ra_pkg 2>/dev/null)
  if [ -n "$PKG" ]; then
    ra_set_pkg "$PKG" 2>/dev/null
    ui_print "已识别 dsh-go 包名：$PKG"
  else
    ui_print "没找到 dsh-go 的 runtime 目录；开机后点『执行』会自动识别，或在 /data/adb/rootagent/app.conf 写 APP_PKG=<包名>"
  fi
fi
exit 0
