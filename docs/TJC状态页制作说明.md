# TJC X2 800×480 阶段2状态页制作说明

本文交付给TJC界面开发人员。阶段2只显示状态，不发送ARM、运动、停止、急停或
任务命令。ESP32使用淘晶驰字符串指令模式，通过UART1修改文本控件的`txt`
属性，每条指令以三个`0xFF`字节结束。

## 1. 工程设置

- 屏幕系列：X2。
- 分辨率：800×480。
- 设备型号：必须与实物标签完全一致。
- 串口：115200、8数据位、1停止位、无校验。
- 页面名称：`status`。
- `status`设为上电默认页面。
- 保持默认字符串指令解析模式，不要启用`recmod=1`。
- 中文静态标签需要在工程中导入对应字库。

阶段2只有一个页面。后续增加控制页面时，应将本页动态状态控件设为全局作用域，
或者同步把ESP `app_config.h` 中的控件名称改为带页面名的形式。

## 2. 必须创建的动态文本控件

控件名称必须区分大小写并与下表完全一致。全部使用“文本”控件，初始文本按表中
填写。

| 控件名 | 初始文本 | ESP可能写入的内容 | 建议宽度 |
| --- | --- | --- | --- |
| `tAp` | `DOWN` | `READY`、`DOWN` | 150 |
| `tNano` | `DOWN` | `CONNECTING`、`UP`、`DEGRADED`、`DOWN` | 180 |
| `tTi` | `DOWN` | `UP`、`DOWN` | 150 |
| `tSystem` | `--` | `DISCONNECTED`、`IDLE`、`CALIBRATING`、`ARMED`、`EXECUTING`、`SAFE_STOP`、`FAULT` | 220 |
| `tArmed` | `--` | `YES`、`NO`、`--` | 120 |
| `tFault` | `--` | `NONE`或具体故障码 | 430 |
| `tDistance` | `--` | 距离整数，单位mm | 180 |
| `tHeading` | `--` | 航向整数，单位mdeg | 180 |
| `tLineError` | `--` | 循迹误差整数，缩放倍率×100 | 180 |
| `tLeftSpeed` | `--` | 阶段2固定为`--` | 180 |
| `tRightSpeed` | `--` | 阶段2固定为`--` | 180 |
| `tProgress` | `--` | 0..100，不带百分号 | 120 |
| `tLapCount` | `--` | MSPM0原始圈计数字段或`--` | 120 |
| `tSegment` | `--` | MSPM0原始段编号或`--` | 120 |

注意：当前Nano—MSPM0 V1.1 STATUS没有左右轮速度，因此两个轮速控件必须显示
`--`，不能根据PWM、距离或其他字段推算。`tLapCount`只显示MSPM0实际提供的原始
字段，不能作为通用“跑N圈”完成条件。

## 3. 建议布局

可按以下结构排版，坐标允许根据字体大小微调：

```text
┌──────────────────────────────────────────────────────────┐
│ 小车地面站状态                                           │
│ AP [tAp]       NANO [tNano]       TI [tTi]               │
├──────────────────────────────────────────────────────────┤
│ SYSTEM [tSystem]                  ARMED [tArmed]          │
│ FAULT  [tFault]                                          │
│ 距离(mm) [tDistance]              航向(mdeg) [tHeading]  │
│ 循迹误差×100 [tLineError]          进度(%) [tProgress]    │
│ 左轮速度 [tLeftSpeed]             右轮速度 [tRightSpeed] │
│ 圈计数 [tLapCount]                段编号 [tSegment]       │
└──────────────────────────────────────────────────────────┘
```

建议静态标签和动态值使用不同颜色；动态控件背景使用深色，确保`SAFE_STOP`、
`EMERGENCY_STOPPED`等长文本不会被截断。

## 4. 可选状态颜色定时器

如需自动改变链路文字颜色，可创建定时器`tmColor`，周期500ms、上电使能，在
定时事件中加入以下逻辑。颜色值分别为绿色2016、黄色65504、红色63488。

```text
if(tAp.txt=="READY")
{
  tAp.pco=2016
}else
{
  tAp.pco=63488
}
if(tNano.txt=="UP")
{
  tNano.pco=2016
}else if(tNano.txt=="DEGRADED")
{
  tNano.pco=65504
}else
{
  tNano.pco=63488
}
if(tTi.txt=="UP")
{
  tTi.pco=2016
}else
{
  tTi.pco=63488
}
```

如果当前USART HMI版本对`else if`格式报错，可改成嵌套`if`，以编辑器实际编译
结果为准。阶段2页面没有按钮，所有文本控件的“发送键值”选项都应关闭。

## 5. ESP发送示例

ESP会发送下列ASCII命令，末尾的三个结束字节不显示在文本中：

```text
tAp.txt="READY"
tNano.txt="UP"
tTi.txt="UP"
tSystem.txt="IDLE"
tArmed.txt="NO"
tFault.txt="NONE"
tDistance.txt="320"
tHeading.txt="-1200"
tLineError.txt="-8"
tLeftSpeed.txt="--"
tRightSpeed.txt="--"
tProgress.txt="25"
```

控件名集中定义在ESP工程的`app_config.h`。队友若修改控件名，必须同步修改该
文件，不能只改HMI工程。

## 6. 联调检查表

1. 屏幕单独上电，所有链路显示DOWN，车辆数值显示`--`。
2. ESP启动热点后，`tAp`显示READY。
3. Nano完成TCP握手后，`tNano`显示UP。
4. Nano未接TI板时，`tTi`显示DOWN、`tSystem`显示DISCONNECTED。
5. 插入TI串口并完成HELLO后，系统、故障、距离、航向和循迹误差更新。
6. 拔出TI串口，`tTi`回到DOWN，数值字段回到`--`。
7. 暂停Nano心跳，约3秒显示DEGRADED，约5秒显示DOWN。
8. 重启ESP和Nano，确认页面无需触摸即可恢复。
9. 确认左右轮速度始终为`--`。
10. 阶段2页面不得产生任何发往ESP的控制事件。

验收时保留USART HMI编译截图、实屏照片和控件名称截图，便于后续控制页面继续
沿用同一套命名规则。
