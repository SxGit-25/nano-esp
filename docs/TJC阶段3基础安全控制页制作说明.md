# TJC X2 800×480 阶段3基础安全控制页制作说明

本文交付给TJC界面开发人员。阶段3新增基础安全控制页，覆盖`arm`、上层
`disarm`、`stop`、`estop`、`clear_fault`和`get_status`。本阶段不制作方向按钮、
手动驾驶、定距运动或预设任务。

TJC只发送语义事件，ESP负责生成`groundStationSessionId`、命令`id`和JSON，
Nano负责命令校验及MSPM0事务映射。TJC不能直接生成或转发Nano—MSPM0二进制帧。

> 无线急停不能替代车体物理急停。Wi-Fi、TCP、ESP或Nano故障都可能导致无线
> 急停延迟或无法送达，实车必须保留可独立动作的物理急停。

## 1. 工程和页面设置

- 屏幕系列：X2。
- 分辨率：800×480。
- 串口：115200、8数据位、1停止位、无校验。
- 保持默认字符串指令模式，不要启用`recmod=1`。
- 建议保持阶段2的`status`为上电默认页面。
- 新增页面名称：`safety`。
- 在`status`页增加本地跳转按钮`bSafety`，释放事件写`page safety`。
- 在`safety`页增加本地返回按钮`bBack`，释放事件写`page status`。
- `bSafety`和`bBack`只执行本地换页，不直接发送控制命令。
- 所有控制按钮的“发送组件ID”选项必须关闭，避免混入`0x65`原生触摸帧。
- 建议在`status`和`safety`页初始化事件中执行`bkcmd=0`，避免属性更新确认帧
  干扰事件流。

如果版面允许，最简单的方案是在现有`status`页增加本阶段控件。若使用独立
`safety`页，新增动态控件应设为全局作用域；当前USART HMI版本若要求跨页带
页面名前缀，则在ESP的`app_config.h`中把对应组件名称配置为例如
`safety.tCmdState`，不要改ESP事件解析协议。

使用独立页面时，`status`和`safety`两个页面都必须在各自的Postinitialize
Event发送界面同步事件：

```text
prints "GS:SYNC",0
printh FF FF FF
```

`GS:SYNC`只让ESP清空TJC显示缓存并重发当前状态，不生成网络命令、不分配命令
`id`，也不能改变车辆状态。

## 2. 动态文本控件

控件名称区分大小写。建议创建以下文本控件：

| 控件名 | 初始文本 | ESP可能写入的内容 | 含义 |
| --- | --- | --- | --- |
| `tNano` | `DOWN` | `CONNECTING`、`UP`、`DEGRADED`、`DOWN` | 沿用阶段2的Nano链路状态 |
| `tTi` | `DOWN` | `UP`、`DOWN` | 沿用阶段2的TI链路状态 |
| `tSystem` | `--` | `IDLE`、`ARMED`、`EXECUTING`、`SAFE_STOP`、`FAULT`等 | TI实际系统状态 |
| `tArmed` | `--` | `YES`、`NO`、`--` | TI实际`armed`状态 |
| `tFault` | `--` | `NONE`、具体故障码或`--` | TI实际故障状态 |
| `tControl` | `--` | `ENABLED`、`LOCKED`、`--` | Nano上层运动命令准入锁 |
| `tCmdId` | `--` | 上层命令ID或`--` | 最近一次普通命令的关联ID |
| `tCmdName` | `--` | `ARM`、`DISARM`、`STOP`、`ESTOP`、`CLEAR_FAULT`、`GET_STATUS` | 最近操作 |
| `tCmdState` | `IDLE` | `SENT`、`ACCEPTED`、`DONE`、`FAILED`、`REJECTED` | 最近命令生命周期 |
| `tCmdDetail` | `--` | 完成原因、错误码和简短说明 | 最近结果详情 |

`tControl`和`tArmed`绝不能合并：

- `tArmed`来自MSPM0状态，表示TI实际报告的ARM状态。
- `tControl`来自Nano，表示Nano是否允许后续上层运动命令进入。
- `DISARM`完成后应显示`tControl=LOCKED`。
- 当前Nano—MSPM0 V1.1没有`DISARM`帧，因此`DISARM`完成后`tArmed`仍可能为
  `YES`。界面不得据此显示“TI已失能”“电机已断电”或类似结论。
- Nano准入锁不能代替`estop`，更不能代替物理急停或物理断电。

`get_status`是独立查询消息，不是`command`，没有命令`id`，Nano不会为它返回
`accepted / done`。ESP可以在本地显示`SENT`，收到新`snapshot`后显示
`DONE / SNAPSHOT_RECEIVED`，此时`tCmdId=--`；这只表示刷新响应已到达，不表示
任何车辆控制动作执行成功。

## 3. 按钮和串口事件

ESP阶段3采用不依赖页面ID、组件ID的严格语义事件。每个事件由ASCII token和
三个结束字节`FF FF FF`组成，大小写必须完全一致。

| 控件名 | 按钮文字 | 触发位置 | 固定ASCII token | TJC事件代码 |
| --- | --- | --- | --- | --- |
| `bArm` | `ARM` | Release Event | `GS:ARM` | `prints "GS:ARM",0`后接`printh FF FF FF` |
| `bDisarm` | `DISARM` | Release Event | `GS:DISARM` | `prints "GS:DISARM",0`后接`printh FF FF FF` |
| `bStop` | `STOP` | **Press Event** | `GS:STOP` | `prints "GS:STOP",0`后接`printh FF FF FF` |
| `bEstop` | `无线急停` | **Press Event** | `GS:ESTOP` | `prints "GS:ESTOP",0`后接`printh FF FF FF` |
| `bClearFault` | `CLEAR FAULT` | Release Event | `GS:CLEAR_FAULT` | `prints "GS:CLEAR_FAULT",0`后接`printh FF FF FF` |
| `bGetStatus` | `刷新状态` | Release Event | `GS:GET_STATUS` | `prints "GS:GET_STATUS",0`后接`printh FF FF FF` |

例如`bArm`的Release Event应为两行：

```text
prints "GS:ARM",0
printh FF FF FF
```

`bStop`和`bEstop`仅在Press Event发送，Release Event留空，避免长按等待释放
导致停车请求延迟。其余四个按钮仅在Release Event发送，Press Event留空。
这样一次点击只生成一个事件，长按也不会连续重复发送。

事件线上的实际字节示例：

```text
GS:ARM          -> 47 53 3A 41 52 4D FF FF FF
GS:DISARM       -> 47 53 3A 44 49 53 41 52 4D FF FF FF
GS:STOP         -> 47 53 3A 53 54 4F 50 FF FF FF
GS:ESTOP        -> 47 53 3A 45 53 54 4F 50 FF FF FF
GS:CLEAR_FAULT  -> 47 53 3A 43 4C 45 41 52 5F 46 41 55 4C 54 FF FF FF
GS:GET_STATUS   -> 47 53 3A 47 45 54 5F 53 54 41 54 55 53 FF FF FF
GS:SYNC         -> 47 53 3A 53 59 4E 43 FF FF FF
```

这些token集中配置在ESP的`app_config.h`。如确需改名，TJC工程和ESP配置必须
同时修改；TJC不得在token中添加空格、换行、引号、组件ID或参数。`GS:SYNC`
是页面同步事件，不是第七种车辆命令。

## 4. 建议布局

```text
┌──────────────────────────────────────────────────────────────┐
│ 基础安全控制                         [返回状态页 bBack]      │
│ NANO [tNano]  TI [tTi]  SYSTEM [tSystem]  FAULT [tFault]    │
│ TI ARMED [tArmed]          NANO CONTROL [tControl]           │
├──────────────────────────────────────────────────────────────┤
│ [ ARM bArm ] [ DISARM bDisarm ] [ CLEAR FAULT bClearFault ] │
│ [ 刷新状态 bGetStatus ] [ STOP bStop ] [ 无线急停 bEstop ]  │
├──────────────────────────────────────────────────────────────┤
│ CMD ID [tCmdId]  CMD [tCmdName]  STATE [tCmdState]          │
│ RESULT [tCmdDetail]                                         │
│ 无线急停不能替代车体物理急停                               │
└──────────────────────────────────────────────────────────────┘
```

布局要求：

- `bEstop`使用红底白字，面积应为本页最大按钮，并与普通命令按钮留出明显间隔。
- `bStop`使用黄色或橙色，不能与`bEstop`使用相同视觉语义。
- “无线急停不能替代车体物理急停”必须永久可见，不能只放在弹窗或帮助页。
- `tCmdDetail`应足够宽，至少能显示`TI_OFFLINE`、
  `EMERGENCY_STOP_APPLIED`、`EMERGENCY_STOPPED`、`USER_CANCELLED`等文本。
- 不要使用切换开关表示ARM或DISARM。按钮只发请求，最终状态必须以ESP回写的
  `tArmed`和`tControl`为准。
- 不要在TJC本地把按钮点击直接改成`DONE`或“成功”。

## 5. 命令结果显示规则

普通命令显示过程：

```text
按钮事件
-> SENT
-> ACCEPTED
-> DONE
```

也可能是：

```text
按钮事件 -> SENT -> ACCEPTED -> FAILED
按钮事件 -> SENT -> REJECTED
```

界面含义：

- `SENT`：ESP已经生成并尝试发送命令，不代表Nano或TI已经收到。
- `ACCEPTED`：命令已通过Nano校验，并在需要时已被TI受理，不代表执行完成。
- `DONE`：命令已完成。
- `FAILED`：已受理但执行失败，详情显示`reason`和可用的`faultCode`。
- `REJECTED`：未进入执行，详情显示`error.code`和简短说明。

每个结果必须按ESP维护的
`(groundStationSessionId, id)`关联。TJC只显示ESP给出的最近结果，不生成或保存
会话号和命令ID，也不能把旧会话结果显示成当前命令成功。

建议显示示例：

```text
tCmdId.txt="21"
tCmdName.txt="ESTOP"
tCmdState.txt="DONE"
tCmdDetail.txt="EMERGENCY_STOP_APPLIED"
```

## 6. 按钮禁用规则

界面禁用只用于减少误操作，不能代替ESP和Nano的命令校验。

| 条件 | ARM | DISARM | STOP | ESTOP | CLEAR FAULT | 刷新状态 |
| --- | --- | --- | --- | --- | --- | --- |
| Nano不是`UP` | 禁用 | 禁用 | 禁用 | **保持可触发** | 禁用 | 禁用 |
| Nano为`UP`、TI为`DOWN` | 禁用 | 禁用 | 禁用 | **保持可触发** | 禁用 | 可用 |
| 普通命令等待结果 | 禁用 | 禁用 | **可用** | **可用** | 禁用 | 禁用 |
| `SAFE_STOP`或故障存在 | 禁用 | 可用 | 可用 | **可用** | 可用 | 可用 |
| 无故障且系统`IDLE` | 可用 | 可用 | 可用 | **可用** | 可禁用 | 可用 |

补充要求：

- `bEstop`的本地触摸状态必须保持启用，不因页面状态、普通命令或链路显示而
  执行`tsw bEstop,0`。Nano断链时它仍可把事件交给ESP，但界面必须显示无法
  送达，不能显示成功。
- `bStop`和`bEstop`不得因普通命令处于`SENT`或`ACCEPTED`而禁用。
- `SAFE_STOP`不能因为重连、刷新状态或进入其他页面自动解除。
- `SAFE_STOP`恢复顺序必须是`CLEAR FAULT -> IDLE -> ARM -> ARMED`。
- 是否允许命令执行最终以Nano返回结果为准；不要仅凭TJC文本决定安全条件。

按钮触摸状态最好由ESP主动发送`tsw 控件名,0/1`控制，禁用时还应同步切换按钮
颜色。如果阶段3 ESP版本尚未提供按钮触摸状态更新，可以暂时保持按钮可点击并
依赖Nano拒绝非法请求，但必须正确显示`REJECTED`，不得在TJC内自行推断执行
成功。

## 7. 分步联调

### 7.1 只测TJC串口事件

先不连接车辆，用串口接收工具逐个检查：

1. 单击每个按钮只收到一次对应token。
2. token后严格只有三个`FF`结束字节。
3. `bStop`和`bEstop`在按下时立即发送，释放时不再发送。
4. ARM、DISARM、CLEAR FAULT和刷新状态在释放时发送，按下时不发送。
5. 长按任一按钮不产生周期重复帧。
6. 换页按钮不产生车辆命令；进入`status`或独立`safety`页时只产生一次
   `GS:SYNC`。
7. 串口中不出现`0x65 pageId componentId event`触摸帧。

### 7.2 ESP—TJC桌面联调

1. ESP收到六种token后都映射到对应语义，未知token被忽略并记录日志。
2. ESP收到`GS:SYNC`后强制重发当前TJC状态，但不向Nano生成命令。
3. `GET_STATUS`生成独立`get_status`消息，不占用普通命令ID。
4. 其余五种命令显示正确的`SENT / ACCEPTED / DONE / FAILED / REJECTED`。
5. 重复点击普通按钮不会把旧ID的结果覆盖到新命令。
6. Nano断链时不能显示`DONE`，链路恢复后旧会话结果不能冒充新命令结果。

## 8. 车轮架空验收清单

实车验收必须将驱动轮可靠架空，车体固定，操作人员站在物理急停可触及位置。
本阶段不发送手动驾驶或运动任务。

1. 上电后确认车辆不运动，链路和TI状态显示正确。
2. 点击“刷新状态”，确认状态更新；不应出现伪造的`accepted`或`done`。
3. 点击ARM，观察`SENT -> ACCEPTED -> DONE`，并以TI回报确认`tArmed=YES`。
4. 点击DISARM，确认活动事务被处理后`tControl=LOCKED`；不得因为DISARM而把
   `tArmed`强制改成`NO`，也不得显示“TI已失能”。
5. 无活动事务时点击STOP，应收到`DONE`；STOP不应锁存`SAFE_STOP`。
6. 在ARM状态按下无线急停，确认事件在Press Event立即发出，普通待执行命令被
   清理，TI最终进入`SAFE_STOP`并报告急停锁存。
7. ESP复位、Nano重连或点击刷新状态，`SAFE_STOP`均不得自动解除。
8. 按`CLEAR FAULT`，确认只有允许软件恢复且故障条件已消失时才回到`IDLE`；
   硬件故障仍存在时应显示`FAILED`或`REJECTED`。
9. 清故障后必须重新ARM，车辆不得自动恢复到可运动状态。
10. Nano断链时，普通按钮应禁用或明确返回无法送达；无线急停按钮仍保持可触发，
    但界面不得宣称车辆已停车。
11. TI串口拔出后`tTi=DOWN`，ARM、DISARM、STOP和CLEAR FAULT不能显示成功。
12. 全程确认车辆没有因开机、换页、刷新、重连或DISARM自行运动。

验收应保存USART HMI编译截图、六种事件的串口字节记录、车轮架空照片、急停及
恢复流程视频，以及命令结果控件截图。HMI二进制由TJC负责人使用与实物一致的
USART HMI环境编译和烧录。
