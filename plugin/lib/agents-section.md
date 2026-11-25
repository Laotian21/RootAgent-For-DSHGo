
## 本机 root 通道与沙箱（Root Agent）

- 真 root：`rootrun '<命令>'`（/usr/local/bin/rootrun）；令牌在 /root/.dsh/rootagent-token，沙箱内被 mask，所以沙箱里用不了 rootrun —— 要用它得升级到 danger-full-access 并获批。
- 沙箱链路：/usr/local/bin/dsh-bwrap-root → socket /root/.dsh/rootagent-sandbox.sock → root 侧 dshsb-exec（mount namespace + chroot + 降权到 App uid）。
- 已知边界：内核可能不支持 PID namespace，隔离靠 uid 降权；/root/.dsh 与 /sdcard/Download/RootAgent 被 mask；Android 的 /data 在沙箱内不可见。
- 排障：bash 报 dsh-bwrap-root 前缀或 exit 111 → 看 /data/adb/rootagent/agent.log；socket 不存在 → 在 SukiSU 点模块「执行」或重启手机。
