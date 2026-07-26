# ESP32-S3 地面站阶段 1（Arduino IDE）

该工程是 TCP Server：ESP32-S3 建立 WPA2 SoftAP，固定地址
`192.168.4.1`，监听端口 `8765`，只接受一个 Nano TCP Client。

当前阶段仅包含网络链路：

- 4 字节小端长度前缀、1..4096 B、UTF-8 JSON 对象。
- `hello / hello_ack`，ESP 每次启动生成一个非零随机
  `groundStationSessionId`，并在其运行期间保持不变。
- 1 秒双向心跳；3 秒显示 DEGRADED，5 秒关闭连接。
- `TCP_NODELAY` 与有界发送队列；没有任何运动、ARM、急停或 MSPM0 串口代码。
- 通过 UART1 向 TJC 设置 `tAp.txt` 和 `tNano.txt`，显示 AP/Nano 状态。

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

默认 TJC 接线是 GPIO4 TX -> TJC RX、GPIO5 RX <- TJC TX、共地。若 HMI
控件名不是 `tAp` 与 `tNano`，在 `app_config.h` 修改两个集中定义。

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
