连接 ESP32 热点：

```bash
sudo nmcli connection up ESP_Nano ifname wlan0
```

确认路由：

```bash
nmcli -f GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY device show wlan0
ip route get 192.168.4.1
ping -c 3 192.168.4.1
```

Nano 地址由 DHCP 分配为 `192.168.4.x`，ESP32 地址和网关固定为
`192.168.4.1`。

启动只读状态桥：

```bash
cd ~/nano_code/ground_station_gateway
python3 gateway_main.py \
  --host 192.168.4.1 \
  --port 8765 \
  --serial-port /dev/serial/by-id/替换为实际设备 \
  --mspm0-link-dir ../mspm0_link
```
