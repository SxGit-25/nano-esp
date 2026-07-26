# Jetson Nano 网络网关与只读状态桥

该目录是独立于小车固件仓库的 ESP32--Nano 通信工程。网络功能可以独立运行；
启用 `--serial-port` 后，由进程内唯一的 `CarSerialWorker` 动态调用并列目录
`mspm0_link` 中已有的 `CarSerialLink`。当前只允许 HELLO、100 ms HEARTBEAT 和
GET_STATUS，不开放任何车辆控制命令。

功能：

- Nano 作为 TCP Client，连接 `192.168.4.1:8765`。
- 4 字节小端长度前缀、UTF-8 JSON 对象及 1..4096 B 长度检查。
- `hello / hello_ack` 握手，保存 ESP 每次启动生成的
  `groundStationSessionId`。
- 每秒网络心跳、`TCP_NODELAY`、3 秒 DEGRADED、5 秒断开、
  0.5/1/2/5 秒重连退避。
- 单一串口线程独占 Nano--MSPM0 UART，每秒读取一次 STATUS。
- 所有对外状态由统一 `StateStore` 生成；握手后先发 `snapshot`，之后发送
  `status`。
- 普通状态只保留最新状态并回传 ESP32，不会在 TCP 断线时累积。

仅验证 ESP32--Nano 网络时运行：

```bash
cd ~/nano_code/ground_station_gateway
python3 gateway_main.py --host 192.168.4.1 --port 8765
```

启用最小状态贯通前，先确认稳定串口路径：

```bash
ls -l /dev/serial/by-id/ 2>/dev/null
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

随后运行：

```bash
cd ~/nano_code/ground_station_gateway
python3 gateway_main.py \
  --host 192.168.4.1 \
  --port 8765 \
  --serial-port /dev/ttyUSB0 \
  --mspm0-link-dir ../mspm0_link
```

Nano 需要安装 `python3-serial`，当前用户需要有串口权限：

```bash
sudo apt install -y python3-serial
sudo usermod -aG dialout "$USER"
```

加入 `dialout` 后需要注销并重新登录。不要同时运行 `link_test.py` 或其他打开
同一串口的程序；整个进程中只有 `CarSerialWorker` 可以访问 UART。

运行测试：

```bash
python3 -m unittest discover -s nano_gateway/tests -v
```

当前 `helloRttMs` 是 Nano 发送 hello 到收到 hello_ack 的链路往返时间；
`reconnect_count` 记录完成握手后的重连次数。ESP--Nano 的 1 秒网络心跳与
Nano--MSPM0 的 100 ms 串口心跳相互独立。

## 开机自动运行

先创建只连接ESP热点、且不抢占默认互联网路由的NetworkManager连接。将示例中
的SSID和密码替换为 `secrets.h` 中的实际配置：

```bash
sudo nmcli connection add type wifi ifname wlan0 con-name ESP_Nano \
  ssid CAR-GS-xxxx
sudo nmcli connection modify ESP_Nano \
  wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk '替换为实际密码' \
  ipv4.never-default yes \
  connection.autoconnect yes
```

当前服务按实机配置使用 `/dev/ttyUSB0`。安装服务前同时确认该设备节点和
`mspm0_link` 目录；如果以后接入多个USB串口，应改用稳定的
`/dev/serial/by-id/...` 路径。随后执行：

```bash
sudo install -m 0755 deploy/esp-nano-wifi-watchdog \
  /usr/local/sbin/esp-nano-wifi-watchdog
sudo install -m 0644 deploy/esp-nano-wifi.service \
  /etc/systemd/system/esp-nano-wifi.service
sudo install -m 0644 deploy/esp-nano-wifi-watchdog.service \
  /etc/systemd/system/esp-nano-wifi-watchdog.service
sudo install -m 0644 deploy/ground-station-gateway.service \
  /etc/systemd/system/ground-station-gateway.service
sudo systemctl daemon-reload
sudo systemctl enable --now esp-nano-wifi.service
sudo systemctl enable --now esp-nano-wifi-watchdog.service
sudo systemctl enable --now ground-station-gateway.service
```

检查：

```bash
systemctl --no-pager --full status \
  esp-nano-wifi.service \
  esp-nano-wifi-watchdog.service \
  ground-station-gateway.service
journalctl -u ground-station-gateway.service -f
```
