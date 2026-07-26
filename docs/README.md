# ESP32--Nano 地面站通信

这是独立于 MSPM0 小车固件的地面站通信工程。阶段 1 的 ESP32--Nano 网络链路
已经完成；当前已实现阶段 2 的只读状态贯通。Nano 通过单一串口工作线程调用
并列目录中的 `mspm0_link`，只执行 HELLO、HEARTBEAT 和 GET_STATUS，不发送
运动、ARM、取消、急停或任务命令。

目录：

- `esp32_ground_station/`：ESP32-S3 SoftAP、单客户端 TCP Server 与 TJC 状态显示。
- `nano_gateway/`：Jetson Nano TCP Client、单串口工作线程、状态汇总与重连。
- `地面站通信与任务控制说明书_V2.0_简化实施版.md`：上层协议与阶段计划。

先运行 Nano 端自动化测试：

```bash
python3 -m unittest discover -s nano_gateway/tests -v
```

随后按各子目录 README 配置 ESP32 和 Nano。ESP32 工程使用 Arduino IDE
编译和烧录。当前验收范围是网络稳定、串口只读状态、状态页显示和掉线恢复，
不是车辆控制。

TJC队友开发状态页时使用 `TJC状态页制作说明.md`。
