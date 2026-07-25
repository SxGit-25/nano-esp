# ESP32-S3 地面站阶段 1

该工程是 TCP Server：ESP32-S3 建立 WPA2 SoftAP，固定地址
`192.168.4.1`，监听端口 `8765`，只接受一个 Nano TCP Client。

当前阶段仅包含网络链路：

- 4 字节小端长度前缀、1..4096 B、UTF-8 JSON 对象。
- `hello / hello_ack`，ESP 每次启动生成一个非零随机
  `groundStationSessionId`，并在其运行期间保持不变。
- 1 秒双向心跳；3 秒显示 DEGRADED，5 秒关闭连接。
- `TCP_NODELAY` 与有界发送队列；没有任何运动、ARM、急停或 MSPM0 串口代码。
- 通过 UART1 向 TJC 设置 `tAp.txt` 和 `tNano.txt`，显示 AP/Nano 状态。

## 首次配置与构建

```bash
cd esp32_ground_station
cp include/secrets.h.example include/secrets.h
# 编辑 secrets.h，设置不提交仓库的 WPA2 密码
pio run
pio run -t upload
pio device monitor
```

默认 TJC 接线是 GPIO4 TX -> TJC RX、GPIO5 RX <- TJC TX、共地。若 HMI
控件名不是 `tAp` 与 `tNano`，在 `include/app_config.h` 修改两个集中定义。

本机未发现 PlatformIO，因此尚不能在此电脑执行 ESP 编译；部署前需安装
PlatformIO，或在 VS Code 的 PlatformIO 环境中构建。
