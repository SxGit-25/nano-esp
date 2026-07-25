# ESP32--Nano 地面站通信

这是独立于 MSPM0 小车固件的阶段 1 通信工程；小车工程只被用作协议能力的
只读参考。本工作区不访问 Nano--MSPM0 UART，不发送运动、ARM、急停或任务命令。

目录：

- `esp32_ground_station/`：ESP32-S3 SoftAP、单客户端 TCP Server 与 TJC 状态显示。
- `nano_gateway/`：Jetson Nano TCP Client、握手、心跳与重连。
- `地面站通信与任务控制说明书_V2.0_简化实施版.md`：上层协议与阶段计划。

先运行 Nano 端自动化测试：

```bash
python3 -m unittest discover -s nano_gateway/tests -v
```

随后按各子目录 README 配置 ESP32 和 Nano。阶段 1 的验收是网络稳定和重连，
不是车辆控制。
