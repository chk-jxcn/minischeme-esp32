# 蓝牙（BLE）研究：分支 feature/bluetooth

状态：研究分支，未合并、未发布。目标是弄清楚在现在的 ESP32（无 PSRAM）上蓝牙要花多少内存、
怎样花得最少，并把应用用的蓝牙 API 定下来，以后搬到内存更大的 ESP32-S3 上直接用。

相关分支：`minischeme-esp32` 和 `minischeme-apps` 的 `feature/bluetooth`。

## 1. 结论

- **协议栈选型**：NimBLE，只开 BLE。经典蓝牙（BR/EDR）需要 Bluedroid，体积和内存都大得多，不考虑。
  ESP-IDF 里没有比 NimBLE 更小的主机协议栈；控制器是芯片厂的二进制库，内存基本固定。
- **这块板上可行的两种用法**：
  1. **只开蓝牙、不开 WiFi**（`bt_only = 1`）：VM 堆 91 KB，系统空闲 24~27 KB，合集也能跑。
     代价是没有网页、没有网络时间、没有联网数据。
  2. **WiFi 和蓝牙同时开**：VM 堆 43 KB，只够一个应用（天气实测用到 34 KB），网页可用。
- **蓝牙编进固件但不开**也有代价：VM 堆少 7 KB，系统空闲少 11 KB（见第 3 节）。
  所以在 ESP32 上不建议把蓝牙合进主线固件；带 PSRAM 的 ESP32-S3 上 WiFi 加蓝牙同开，
  VM 堆仍是满额 128 KB（实测，见第 7 节）。
- **协议栈再精简**（不要配对加密、一条连接、缓冲区减小）还能省约 7.6 KB 内存、35 KB 固件，
  代价是不能连需要配对的设备。默认配置没有这样做。

## 2. 实测数据（COM42，ESP32-D0WD，IDF 6.1）

| 配置 | VM 堆 | 加载后系统空闲 | 协议栈占用 | 备注 |
|---|---|---|---|---|
| 主线固件 v1.0.99（没有蓝牙） | 111 KB | 35 KB | - | 基准 |
| 蓝牙编进固件，`bt_enabled = 0` | 104 KB | 24 KB | 0（内存已归还） | IRAM 被控制器代码占掉 23 KB |
| WiFi + 蓝牙，天气 | 43 KB | 32 KB | 26.8 KB | 网页可用；应用用到 34 KB |
| WiFi + 蓝牙，精简协议栈，天气 | 48 KB | 32 KB | 19.2 KB | 无配对、一条连接 |
| 只开蓝牙，合集 | 91 KB | 24 KB | 23.6 KB | 应用用到 37 KB |
| 只开蓝牙，天气 | 91 KB | 27 KB | 23.6 KB | 没有网络，取不到数据 |

说明：

- "协议栈占用"是控制器加 NimBLE 主机启动时拿走的堆。和 WiFi 一起开时多 3 KB（共存模块）。
- WiFi 启动本身占约 33 KB。只开蓝牙时这部分省下来，另外系统预留从 60 KB 降到 48 KB
  （`BT_ONLY_RESERVE`，没有 WiFi 缓冲区和网页服务）。预留降到 32 KB 时 VM 堆有 104 KB，
  但系统空闲只剩 8 KB，太少。
- 固件体积：蓝牙约 250 KB。为了放进 1600 KB 的分区，IDF 的库改成按体积优化（`-Os`），
  断言不带文件名和表达式字符串，去掉错误名称表；解释器、显示和文件系统三个组件仍按 `-O2`。
  结果 1589 KB，分区剩 47 KB。

## 3. 内存花在哪里

- **控制器保留区**：链接时保留约 56 KB DRAM 给双模控制器。`bt_enabled = 0` 时开机调用
  `esp_bt_mem_release` 全部归还（约 44 KB 回到堆里，但和主堆不连续）；开蓝牙时先归还
  经典蓝牙那部分。
- **IRAM**：控制器有 23 KB 代码必须放在 IRAM。主线固件把 VM 的栈、调用帧、显示列表等
  放在 IRAM 的空闲部分（38 KB），蓝牙一编进来 IRAM 只剩 15 KB，放不下的部分挤到普通内存。
  这就是"编进去不开也少 11 KB"的原因，运行时无法归还。
- **主机和控制器的堆**：23.6 KB（两条连接、带配对），没有再细分。
- **事件队列和扫描表**：2.7 KB，蓝牙启动时才分配。

## 4. 启动顺序上的处理

- 蓝牙在 VM 堆定大小之前启动，所以它的内存从 VM 的份额里出，不占系统预留。
- 启动期间 VM 预占的大块先让出来，协议栈起来后再占回去，而且不超过 VM 最终能拿到的大小。
  （最初整块占着，WiFi 加蓝牙时系统只剩 84 字节，网页服务起不来。）
- 应用的 ROM 镜像需要重建时，预占块再让出 24 KB 连续内存给它（否则合集重建会失败）。
- 系统设置永远只用 WiFi、不开蓝牙，所以不管蓝牙怎么配，都能进设置把它改回来。

## 5. 配置和 API

`config.ini`：

```ini
[bluetooth]
bt_enabled = 1     ; 开机启动 BLE 协议栈（默认 0）
bt_only = 1        ; 应用运行时不开 WiFi（默认 0）
bt_name =          ; 广播名，默认 Desk-XXXX
```

应用 API 是 `ble` 扩展（1.0），完整说明在 `docs/api/v1.18.0.md`，规范在 minischeme-apps 的
`tools/api/spec/ext_ble.py`。所有调用都不阻塞：调用只是发起操作，结果以事件到达，
用 `(ble-event)` 取，或者应用定义 `(app-ble-event ev)` 由固件调用。

| 类别 | 接口 |
|---|---|
| 状态 | `ble-status` `ble-event` |
| 扫描 | `ble-scan-start` `ble-scan-stop` `ble-scan-results` `ble-adv-field` |
| 连接外设 | `ble-connect` `ble-disconnect` `ble-connections` `ble-discover` `ble-services` `ble-read` `ble-write` `ble-subscribe` |
| 配对 | `ble-pair` `ble-bonds` `ble-unpair` |
| 当外设 | `ble-advertise` `ble-advertise-stop` `ble-uart-write`（Nordic 串口服务） |
| 数据 | `ble-bytes->hex` `ble-hex->bytes` |
| 预留（名字和参数已定，未实现） | `ble-service-add` `ble-value-set!` `ble-hid-connect` `ble-mtu` `ble-rssi` |

另外新增 `(time-set! seconds)`（API 1.18）：只开蓝牙时没有网络校时，由手机把时间发过来再设置。

例子：读米家温湿度计（LYWSD03MMC）。

```scheme
(define thermo "a4:c1:38:4c:87:a2")
(define conn #f)
(define handle #f)

(define (app-ble-event ev)
  (let ((kind (car ev)))
    (cond ((eq? kind 'connected) (set! conn (cadr ev)) (ble-discover conn))
          ((eq? kind 'discovered)             ; find ebe0ccc1-..., subscribe to it
           (set! handle (find-characteristic (ble-services conn) "ebe0ccc1"))
           (ble-subscribe conn handle))
          ((eq? kind 'notify)                 ; "de0a51990b": 27.82 C, 81 %, 2969 mV
           (show-measurement (ble-hex->bytes (cadddr ev))))
          ((eq? kind 'disconnected) (set! conn #f) (ble-connect thermo)))))

(ble-connect thermo)
```

## 6. 已验证和未验证

已在板上验证（WiFi + 蓝牙模式，通过 `/api/exec`）：

- 扫描：6 秒看到 6 台设备，包括名字、信号强度、是否可连接。
- 连接米家温湿度计、发现服务（10 个服务 40 个特征以内）、读取、订阅通知：通知
  `de0a51990b` 解出 27.82°C、81%、2969 mV。
- 断开；连接超时（10 秒）会以 `(disconnected #f 13)` 事件报告。
- 广播：状态变为 advertising。
- `api-version` 为 1.18.0，`(api-extensions)` 含 `ble 1.0.0`，`api-has?` 对预留接口返回 `#f`。

两块板互连（ESP32 做主机，ESP32-S3 做外设，脚本 scratchpad 的 `two_boards.py`）：

- S3 `(ble-advertise #t)`，ESP32 按名字过滤扫描到它，连接、发现到串口服务（6e400001，写 16、通知 18）。
- ESP32 订阅后写入，S3 收到 `(uart 2 "hello ESP32!!")`；S3 `(ble-uart-write ...)`，ESP32 收到通知。
  两个方向都通，断开后两边都收到 `disconnected`。

未验证和已知问题：

- 手机或电脑做主机连板子（只测了板子对板子）。
- 配对和绑定（`ble-pair`）：手边没有需要配对的 BLE 设备参与测试。
- 只开蓝牙模式下的长时间运行和内存最低值。

修过的问题：

- **连接失败后卡在 `connecting`。** 连接在建立过程中断掉（原因 0x3E）时，协议栈只报一个断开事件，
  `ble.c` 自己记的"正在连接"标志没有清，之后 `ble-connect` 一直返回 `#f`。现在状态一律问协议栈
  （`ble_gap_conn_active` / `disc_active` / `adv_active`），这种断开报成 `(disconnected #f 原因)`。
- **0x3E 本身。** ESP32 做主机连 S3 时 12 次里有 6 次。S3 的主机默认会自动重连（最多 3 次），
  ESP32 默认不会：`sdkconfig.defaults.esp32` 打开 `BT_NIMBLE_ENABLE_CONN_REATTEMPT` 后 16 次全部
  连上。另外 WiFi 和蓝牙同开时 WiFi 改用 modem sleep（原来固件把 WiFi 省电关了，蓝牙抢不到射频）。
- **按名字 / 服务过滤的扫描找不到一部分设备。** 很多设备把名字放在扫描应答里（LYWSD03MMC 就是），
  而过滤只看广播包，应答单独来时又不建行。现在有过滤条件时，匹配的扫描应答也能建行。
- 测试脚本：scratchpad `conn_stress.py [轮数] [swap]`（不存在的地址、取消、扫描中连接、N 轮连接断开）。

## 7. ESP32-S3 实测

板子：微雪 ESP32-S3-LCD-1.47（ESP32-S3R8：16 MB flash，8 MB 八线 PSRAM，ST7789 172x320 SPI 屏，
只有芯片自带的 USB 口）。同一份源码按芯片编译：

```
idf.py -B build_s3 -DSDKCONFIG=sdkconfig.s3 -DIDF_TARGET=esp32s3 build
esptool --chip esp32s3 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 minischeme_esp32.bin
```

| 配置（WiFi + 蓝牙，系统设置应用） | VM 堆 | 内部内存空闲 | PSRAM 空闲 |
|---|---|---|---|
| 不用 PSRAM | 71 KB | 24 KB | - |
| 用 PSRAM，VM 堆留在内部内存 | 63 KB | - | 8 MB |
| 用 PSRAM，VM 堆放 PSRAM（现在的做法） | 128 KB 时测的；现在默认 1 MB | 88 KB（最低 47 KB） | 8 MB（1 MB 堆时 7.3 MB） |

结论和原先的估计不一样的地方：

- **S3 不带 PSRAM 时并不宽裕。** 内部堆一共 284 KB，WiFi 的 IRAM 代码和它共用同一块内存；
  S3 的 BLE 5 控制器加主机占 52~55 KB（ESP32 上是 24~27 KB）。不用 PSRAM 时 WiFi 加蓝牙后
  VM 只有 71 KB。要买带 PSRAM 的型号。
- **VM 堆放 PSRAM 代价很小。** 同一段测试（毫秒）：递归 fib 145 / 145，建表求和（分配和 GC 多）
  207 / 236，向量循环 222 / 222（内部内存 / PSRAM）。eden、VM 栈和调用帧仍在内部内存。
- 蓝牙不开时没有 ESP32 那种"编进去也少内存"的问题（S3 没有 IRAM 专用堆可挤）。
- 蓝牙各接口在 S3 上不用改：扫描、连接温湿度计、发现、读、订阅通知、做外设收发都通过
  （第 6 节）。

移植时改的地方（都在本分支）：

- `components/minischeme/src/minischeme_board_s3.c`：这块板的屏、背光、BOOT 键、RGB 灯，接口和
  `minischeme_ili9488.c` 相同，CMake 按芯片二选一。屏只有 172x320，默认分辨率跟着改了；
  现有应用按 320x480 排版，在这块屏上只是能显示，不是适配。
- 串口控制台走芯片的 USB（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`），`@INFO`、`@READ` 等命令照常。
- `sdkconfig.defaults` 拆成公共部分加 `sdkconfig.defaults.esp32` / `.esp32s3`；S3 用
  `partitions_s3.csv`，不做体积优化，任务栈放大（主任务 9216 在 S3 上会溢出）。
- `MALLOC_CAP_EXEC` 在 S3 上不存在：`mini_word_alloc` 改为取内部内存；`alloc_aligned8` 把大块
  （VM 堆）放 PSRAM；`main.c` 里量 VM 堆大小的地方改用 `VM_HEAP_CAPS` / `vm_side_free()`。
- `esp_bt_controller_mem_release(CLASSIC_BT)` 只在 ESP32 上调用。

VM 堆大小：S3 上默认 `max_heap` 是 1 MB（`config.ini [memory] max_heap`，最大 4 MB），整块在 PSRAM。
新建 `config.ini` 时模板里 ESP32 的值（320x480、131072）会被换成这块板的默认值。实测：堆里放
950 KB 数据系统稳定；700 KB 存活数据时一次完整回收最长停顿 62 毫秒；堆大了以后回收次数少，
建表求和的测试从 236 毫秒降到 69 毫秒。

还没做：

- 预留的 5 个接口（自定义 GATT 服务、HID 遥控器、MTU、信号强度）。
- S3 控制器的内存配置没有精简（连接数、缓冲区都是默认值）。
- 另一种 S3 板（4 MB flash、2 MB 四线 PSRAM 的小板）需要自己的 `sdkconfig` 和分区表。

### 垃圾回收：PSRAM 上的老生代用 mark-sweep

有 PSRAM 的构建（`CONFIG_SPIRAM`）自动打开 `MINI_GC_MARKSWEEP`，没有 PSRAM 的 ESP32 不变（仍是
滑动压缩）。核心库的改动在 minischeme 的 `feature/gc-marksweep` 分支，这里的
`components/minischeme` 是它的拷贝。

- 新生代（eden）不变。老生代的对象不再搬动：一次完整回收只做标记，清扫分摊到之后的每次小回收
  （每次 512 个块）和等空间的分配上。晋升的对象优先放进大小正好的空洞，其余放进事先确认够大的
  预留块（可以由几个空洞拼成）。
- 堆要增长，或者空闲空间碎到放不下时，退回原来的压缩式回收。`(vm-stats)` 里 `compact-gcs` 是
  压缩的次数，`gc-max-us` 是最长一次停顿（`(vm-stats #t)` 读完清零），`gc-total-ms` 是累计时间。
- 顺带修了一个原有问题：堆固定大小且存活数据超过 80% 时，每次小回收后面都跟一次完整回收。

实测最长停顿（S3，单位毫秒；"原来"是压缩式回收）：

| 堆 | 存活数据 | 原来 | 现在 |
|---|---|---|---|
| 4 MB | 3 MB，1500 个向量 | 165（观察到 355） | 78 ~ 83 |
| 4 MB | 1.9 MB，12 万个 pair | 315 | 58 ~ 60 |
| 4 MB | 丢掉上面的数据之后 | 103 | 5.7 |
| 1 MB | 只有应用（约 110 KB） | — | 4.9 |
| 1 MB | 640 KB，向量 | 62（700 KB 时） | 19.5 |
| 1 MB | 630 KB，pair | — | 32 |

以上测试里压缩次数都是 0。停顿仍然和存活数据成正比（每 MB 约 25 ~ 30 毫秒），要和数据量无关
得做增量标记（还没做）。堆接近放满时回收会很频繁并退回压缩，停顿到 150 ~ 200 毫秒。

测试：核心库 `make test` 和 `make GCMS=1 test`（`tests/test_gc.c` 两种回收器都跑；
`EXTRA=-DMINI_GC_VERIFY` 每次回收后校验整个堆）。板上：scratchpad `s3run.py` + `gc_cases8.txt`。
注意测试脚本的全局变量别和应用库重名（`big` 是 applib 画大字的函数，覆盖后应用渲染报错重启）。

## 8. 示例应用：蓝牙温湿度（thermo）

在 minischeme-apps 的 `feature/bluetooth` 分支：`apps/thermo/`。读米家温湿度计 LYWSD03MMC：
按名字扫描（或用设置里的地址）、连接、发现、订阅 ebe0ccc1、取第一个通知后断开，每隔 `interval`
分钟一次（默认 5，0 为一直连着）；显示温度、湿度、电量、更新时间和温度曲线。每一步有超时，
失败后 15 秒重试。用 `app-ble-event` 和 `app-step-proc`，没有 `ble` 扩展或蓝牙没开时只显示提示。

版面按屏幕宽度排：172 像素宽时上下排，280 像素以上左右排。在 S3 板（172x320）和 COM42（320x480，
蓝牙固件，WiFi 加蓝牙）上都自动搜到并读出了数据。安装：
`python tools/board.py <ip> install thermo --boot`。

ESP32 上 WiFi 加蓝牙跑这个应用很紧：VM 堆 37 KB 用了 26 KB，系统空闲最低到过 2.4 KB。
