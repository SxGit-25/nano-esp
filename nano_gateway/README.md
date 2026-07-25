# Jetson Nano 阶段 1 网络网关

该目录是独立于小车固件仓库的 ESP32--Nano 通信工程。它不导入、修改或访问
Nano--MSPM0 的串口代码，因此不会在阶段 1 控制车辆。

功能：

- Nano 作为 TCP Client，连接 `192.168.4.1:8765`。
- 4 字节小端长度前缀、UTF-8 JSON 对象及 1..4096 B 长度检查。
- `hello / hello_ack` 握手，保存 ESP 每次启动生成的
  `groundStationSessionId`。
- 每秒网络心跳、`TCP_NODELAY`、3 秒 DEGRADED、5 秒断开、
  0.5/1/2/5 秒重连退避。

在 Nano 已接入 `CAR-GS-xxxx` 后运行：

```bash
cd nano_gateway
python3 gateway_main.py --host 192.168.4.1 --port 8765
```

运行测试：

```bash
python3 -m unittest discover -s nano_gateway/tests -v
```

当前 `helloRttMs` 是 Nano 发送 hello 到收到 hello_ack 的链路往返时间；
`reconnect_count` 记录完成握手后的重连次数。网络心跳不替代未来 Nano--MSPM0
串口工作线程必须维持的 100 ms 安全心跳。
