# ESP32--Nano 地面站通信

这是独立于 MSPM0 小车固件的地面站通信工程。阶段 1 网络链路和阶段 2 状态
贯通已经完成；当前已实现阶段 3 基础安全命令。Nano 通过单一串口工作线程调用
并列目录中的 `mspm0_link`，支持 ARM、上层 DISARM、STOP、无线急停、清故障和
状态查询，不支持手动驾驶、定距、转角、循迹或预设任务。

目录：

- `esp32_ground_station/`：ESP32-S3 SoftAP、单客户端 TCP Server 与 TJC 状态显示。
- `nano_gateway/`：Jetson Nano TCP Client、单串口工作线程、状态汇总与重连。
- `地面站通信与任务控制说明书_V2.0_简化实施版.md`：上层协议与阶段计划。

先运行 Nano 端自动化测试：

```bash
python3 -m unittest discover -s nano_gateway/tests -v
```

随后按各子目录 README 配置 ESP32 和 Nano。ESP32 工程使用 Arduino IDE
编译和烧录。阶段3实机验收必须先不接电机，再将车轮可靠架空；无线急停不能
替代车体物理急停。

TJC界面制作说明：

- `TJC状态页制作说明.md`：阶段2只读状态页。
- `TJC阶段3基础安全控制页制作说明.md`：阶段3 ARM、上层DISARM、STOP、
  无线急停、清故障、刷新状态及命令结果页。
