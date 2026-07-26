# ESP32-S3 地面站网络与状态接收（Arduino IDE）

该工程是 TCP Server：ESP32-S3 建立 WPA2 SoftAP，固定地址
`192.168.4.1`，监听端口 `8765`，只接受一个 Nano TCP Client。

当前阶段包含网络链路和最小只读状态接收：

- 4 字节小端长度前缀、1..4096 B、UTF-8 JSON 对象。
- `hello / hello_ack`，ESP 每次启动生成一个非零随机
  `groundStationSessionId`，并在其运行期间保持不变。
- 1 秒双向心跳；3 秒显示 DEGRADED，5 秒关闭连接。
- `TCP_NODELAY` 与有界发送队列；没有任何运动、ARM、急停或 MSPM0 串口代码。
- 接收 Nano 握手后的 `snapshot` 和周期 `status`，保存系统状态、故障、距离、
  航向和循迹误差，但不接收或转发任何控制命令。
- 通过UART1向TJC显示AP/Nano/TI链路、系统状态、armed、故障、距离、航向、
  循迹误差、进度和事件计数；V1.1没有的左右轮速度显示`--`。

## Arduino IDE 环境

1. 安装 Arduino IDE 2.x。
2. 在“文件 -> 首选项 -> 其他开发板管理器地址”中加入：

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. 在“开发板管理器”中安装 Espressif Systems 的 `esp32` 开发板包。
4. 在“库管理器”中安装 Benoit Blanchon 的 `ArduinoJson` 6.21.5。

## 首次配置

将同目录的 `secrets.h.example` 复制为 `secrets.h`，然后设置热点名称和
WPA2 密码：

```cpp
#define CAR_GS_WIFI_SSID "CAR-GS-xxxx"
#define CAR_GS_WIFI_PASSWORD "replace-with-a-private-password"
```

`secrets.h` 已加入 `.gitignore`，不要提交真实密码。

## 编译与烧录

1. 用 Arduino IDE 打开 `esp32_ground_station.ino`。
2. 选择“工具 -> 开发板 -> esp32 -> ESP32S3 Dev Module”。对于标准
   ESP32-S3-DevKitC-1，也使用这个开发板配置。
3. 选择 ESP32-S3 对应的串口。
4. 点击“验证”编译，再点击“上传”烧录。
5. 打开串口监视器，波特率选择 `115200`。

如果上传一直等待连接，按住开发板 `BOOT`，短按一次 `RESET`，开始上传后松开
`BOOT`。具体是否需要手动进入下载模式取决于开发板的自动复位电路。

阶段2状态页只需 GPIO4 TX -> TJC RX 并共地，TJC TX 返回线暂不连接。
GPIO6 保留给后续返回通道联调。HMI状态页必须使用
`docs/TJC状态页制作说明.md` 中的控件名称；如需改名，在 `app_config.h`
同步修改集中定义。

## 串口屏通信最高优先级原则（已验证故障记录）

> 这是本工程后续所有TJC串口屏修改和故障排查的最高优先级约束。先保证
> ESP32到TJC的状态发送能够独立工作，再处理TJC到ESP32的返回、触摸和控制
> 通道。返回通道异常不得阻塞状态显示。

2026-07-27实物联调中，串口屏长期保持`DOWN`。最终确认接线、波特率、HMI
控件和单条字符串指令均正常。以下指令已在USART HMI模拟器和实物屏幕分别
验证成功：

```text
tAp.txt="UART OK" FF FF FF
```

实际发送的是ASCII指令，末尾紧跟三个二进制`0xFF`字节，不包含日志前缀、
空格、`\r`或`\n`。完整字节为：

```text
74 41 70 2E 74 78 74 3D 22 55 41 52 54 20 4F 4B 22 FF FF FF
```

本次故障不是字符串赋值语法错误，也不是TJC完全不支持返回。真正的问题是
曾把双向回读验证设计成了单向状态发送的前置条件：

```text
发送 bkcmd=3
-> 写入探测文本
-> 发送 get tAp.txt
-> 等待 0x70 字符串返回帧
-> 只有 readback_verified=true 才发送 READY/UP/DOWN
```

实物返回通道当时没有收到预期的`0x70`帧，而是出现发送数据回显。因此
`readback_verified`始终为假，ESP32明明已经取得AP、Nano和TI的真实状态，
却被自身门控逻辑禁止发送，HMI只能保留初始`DOWN`。

阶段2状态页必须采用以下发送策略：

- ESP32使用UART1、115200、8N1，从GPIO4发送到TJC RX，并可靠共地。
- 状态变化时立即发送对应的`txt`赋值指令。
- 每1秒全量重发一次所有已知状态，覆盖屏幕晚启动、启动页期间丢包和屏幕
  重启。
- 不以`bkcmd`成功码、`get`读回、`online`、`ready`或
  `readback_verified`作为状态发送条件。
- ESP32不为同步状态而强制切换HMI页面；启动动画和页面切换由HMI负责。
- 阶段2不需要TJC TX返回线。GPIO6只作为后续独立联调返回通道的预留引脚。

上述约束不是“一棍子打死”所有双向通信。`bkcmd=3`、`get`、TJC TX、
触摸事件和自定义`printh`帧仍然有价值，但必须区分用途：

- `bkcmd`和`get`可以用于诊断屏幕是否执行命令，但诊断失败只能降低链路
  健康度，不能停止状态发送。
- 后续前进、后退、停止等控制页面可以启用TJC TX，并使用独立、明确的
  二进制控制帧。
- 控制命令如需确认，应只约束该控制事务，不能反向锁死ESP32到TJC的状态
  显示通道。
- “ESP32到TJC发送成功”不代表“TJC到ESP32返回正常”；同样，返回异常也
  不能直接判定发送协议、接线和HMI全部错误。

串口屏出现问题时按以下顺序隔离，不要同时否定所有层：

1. 先确认观察对象和串口方向。`Serial`是USB/COM调试口，`Serial1`才是
   GPIO4上的TJC发送口；电脑模拟器成功不等于GPIO4物理路径已经验证。
2. 核对原始字节，而不是串口日志文字。字符串引号必须是ASCII
   `0x22`，结束符必须是三个真正的`0xFF`。
3. 先用单条`tAp.txt="UART OK"`验证解析器，再测试完整状态字段。
4. 实物测试先只连接GPIO4到TJC RX并共地，暂时断开TJC TX，排除回显和
   返回通道影响。
5. 需要验证返回方向时，让TJC通过定时器或按钮主动发送已知
   `printh`测试帧，独立验证TJC TX到GPIO6；不要用返回结果阻塞发送方向。
6. 只有原始字节证据明确指向某一层时，才修改该层的协议、代码、HMI或
   接线。

## Arduino 草图结构

```text
esp32_ground_station/
├── esp32_ground_station.ino
├── app_config.h
├── secrets.h.example
├── secrets.h                 # 本地创建，不提交
└── src/
    ├── network/
    │   ├── framed_json.h
    │   ├── framed_json.cpp
    │   ├── tcp_server.h
    │   └── tcp_server.cpp
    └── tjc/
        ├── tjc_display.h
        └── tjc_display.cpp
```

Arduino IDE 会递归编译草图 `src/` 下的 `.cpp` 文件。
