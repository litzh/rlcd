# BLE 配网协议与验证

设备名为 `RLCD-` 加 MAC 尾四位。设备只广播一个自定义服务：

| 用途 | UUID | 权限 |
| --- | --- | --- |
| 配网服务 | `98bf0001-7d56-4f36-913a-2f0743a89c11` | — |
| 配置输入 | `98bf0002-7d56-4f36-913a-2f0743a89c11` | Write with response |
| 状态 | `98bf0003-7d56-4f36-913a-2f0743a89c11` | Read |

输入是一行 UTF-8 JSON：`{"ssid":"...","password":"..."}\n`。页面按 20 字节分段写入，适配最小 ATT MTU；设备按换行组包，最大 1023 字节，不完整包不执行。发送新配置前先写一个换行，清理之前中断的包。输入队列深度为 2，客户端应等待当前连接结果后再提交下一条。

页面每秒读取一次状态，避免长通知超过 MTU；GATT 读取和写入串行执行。返回：

```json
{"state":"connected","result":"saved","ip":"192.168.1.100","provisioning":false}
```

结果包括 `idle`、`connecting`、`saved`、`connection_failed`、`invalid_credentials`、`busy`、`save_failed`、`provisioning_closed`。

Wi-Fi 状态机在主循环运行，蓝牙回调只组包并向队列提交，不执行联网或存储。新网络获得 IP 后将 SSID/密码作为单个 NVS 字符串提交。连接失败保持原存储。成功停止广播，已有连接允许读结果但拒绝新配置，直到本机再次开放配网。

## 验证步骤

1. 初次烧录且无保存配置：20 秒内连接默认测试网络，屏幕显示 IP。
2. 长按 BOOT 三秒，用 HTML 发现并连接 RLCD；确认可读取当前状态。
3. 提交不存在的 Wi-Fi：等待 20 秒，应返回 `connection_failed` 并继续允许配网。
4. 提交有效 Wi-Fi：得到 IP 后返回 `saved`；重启后 `/status` 的 `wifi.source` 为 `saved`。
5. 再提交错误网络并重启：原保存配置仍然可用。
6. 配网发送中断后重新连接，重新提交有效配置，应能恢复。
7. 验证 HTTP 接口；确认屏幕显示 IP、英文消息和传感器读数。
8. 断开路由器后设备重连失败应回到配网状态；恢复路由器后通过配网或重启连接。

NVS namespace 为 `rlcd-wifi`，key 为 `credentials`。本版本没有远程清除配置接口，普通烧录保持已保存配置。
