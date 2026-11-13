# LED 矩阵板（Waveshare ESP32-S3-Matrix）

研究分支 `feature/bluetooth` 上的第三块板：ESP32-S3FH4R2（4 MB flash、2 MB 四线 PSRAM）、
8×8 RGB LED 矩阵（WS2812，数据脚 GPIO 14）、QMI8658 姿态传感器、没有屏幕、只有 BOOT 和 RESET
两个键。没有合并到 main，没有发布。

## 1. 固件怎么跑

- **没有屏幕**：板级文件 `minischeme_board_matrix.c` 里显示接口全部是空操作，GUI 画到一块
  172×320 的虚拟屏幕上（和另一块 S3 板一样大）。应用照常运行，WebREPL 的镜像和截图
  （`/api/screen`）能看到画面。
- **LED 矩阵**：RMT 外设加 DMA 发送，一帧 64 颗约 2.4 毫秒，不关中断。其它板上的单颗状态灯
  （`ws2812-set`）在这块板上是整个矩阵一起亮。开机后左上角一颗青色表示固件起来了。
- **内存**：VM 堆 1 MB 放在 PSRAM（老生代用 mark-sweep），启动后系统空闲约 1.1 MB。
- **按键**：BOOT（GPIO 0）就是应用里的那个按键。
- 姿态传感器 QMI8658：I2C，SDA 11、SCL 12，地址 0x6B（板上实测找到），见第 7 节。

## 2. 构建和烧录

```
idf.py -B build_matrix -DSDKCONFIG=sdkconfig.matrix -DIDF_TARGET=esp32s3 -DMINI_BOARD=matrix build
esptool --chip esp32s3 -p COMx write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 minischeme_esp32.bin
```

- `-DMINI_BOARD=matrix` 选板级文件、加上 `matrix.c`（API）、定义 `MINI_BOARD_MATRIX`，并在
  `sdkconfig.defaults` / `sdkconfig.defaults.esp32s3` 之上再读 `sdkconfig.defaults.matrix`
  （4 MB flash、`partitions_matrix.csv`、四线 PSRAM、按体积优化）。
- 固件 1.60 MB，应用分区 1792 KB（余 13%）；字库 320 KB、应用 ROM 512 KB、文件系统 1408 KB。
- 出厂固件备份：`scratchgen/matrix_board/factory_backup.bin`（4 MB，`write_flash 0` 可恢复）。
- 进下载模式：按住 BOOT 再按 RESET。

## 3. 给应用用的 API：`matrix` 扩展 1.0（API 1.19）

坐标 `(x, y)`，左上角是 `(0, 0)`；颜色是一个数 `#xRRGGBB`。`matrix-set`、`matrix-fill` 只改
缓冲区，`(matrix-show)` 才送到 LED；`matrix-draw`、`matrix-clear`、`matrix-brightness` 立即生效。

| 过程 | 作用 |
|---|---|
| `(matrix-size)` | `(8 . 8)` |
| `(matrix-set x y color)` | 改缓冲区里一个点 |
| `(matrix-get x y)` | 读缓冲区里一个点 |
| `(matrix-fill color)` | 缓冲区全部设成一个颜色 |
| `(matrix-show)` | 把缓冲区送到 LED |
| `(matrix-draw colors)` | 64 个颜色的向量或列表（从左上角一行一行），设置并显示 |
| `(matrix-clear)` | 全灭 |
| `(matrix-brightness [percent])` | 读 / 设亮度 0..100 |

```scheme
(if (api-has? "matrix-set")
    (begin
      (matrix-brightness 20)
      (matrix-fill 0)
      (matrix-set 0 0 #xff0000)        ; 左上角红
      (matrix-set 7 7 #x0000ff)        ; 右下角蓝
      (matrix-show)))
```

应用在 `meta.ini` 里写 `extensions = matrix 1.0` 声明需要它；别的板上 `(api-has? "matrix-set")`
是 `#f`。参数越界是错误（`matrix-set: expected x (0..7), ...`）。

**亮度和发热**：64 颗全亮白色要几安培电流，几秒钟板子就烫手（微雪的文档也警告过）。固件里每个
颜色通道最多只驱动到 80/255（`MINI_MATRIX_MAX_LEVEL`），`matrix-brightness` 的 100% 就是这个
上限，默认 25%。

## 4. config.ini

```ini
[matrix]
matrix_brightness = 25        ; 开机亮度 0..100
matrix_order = rgb            ; LED 的颜色顺序：rgb（默认）或 grb
matrix_layout = progressive   ; 每行同方向（默认）或 serpentine（隔行反向）
matrix_rotate = 180           ; 0 / 90 / 180（默认）/ 270，顺时针
```

这几个键新建的 config.ini 里没有，要用时自己加。默认值来自用户对测试图案的观察（2026-11-14）：
最初按普通 WS2812 矩阵猜的 grb / 不旋转，实际看到的是整幅倒置、红绿互换，所以默认改成
rgb / 180。板子换个方向摆放时改 `matrix_rotate`，隔行左右颠倒改 `matrix_layout`。

## 5. 验证过的和没验证的

- 验证过（板子 COM62，192.168.2.153）：启动、WiFi、应用加载、8 个过程的返回值和报错、
  100 帧 239 毫秒。
- LED 的位置和颜色：第一次的测试图案用户看到的是倒置且红绿互换，默认值已改（见第 4 节）；
  改后的图案（左上角连续三颗红、右上角一颗绿、左下角一颗蓝、右下角一颗白）还要用户再看一眼。
- 没做：蓝牙在这块板上的测试。

## 6. 示例应用：矩阵灯（matrix）

在 minischeme-apps 的 `feature/bluetooth` 分支：`apps/matrix/`（`matrix.scm`、`meta.ini`、`web.html`）。
安装：`python tools/build_apps.py matrix`，然后 `python tools/board.py <ip> install matrix --boot`
（一定要带 `--boot` 重启：只上传不重启时设备还按旧镜像的位置读网页，网页会被截断，一直显示“读取中”）。

- **八个页面**，一页一种模式：时钟（3×5 数字滚动显示时间，最下一行是秒）、彩虹、数字雨、生命游戏、
  火焰、画板、跑马灯、小球。
- **模式**（设置 `mode`）：`auto` 时框架的轮播（默认每页 10 秒）轮流显示，BOOT 键换下一个；
  选某一页则一直显示它（`app-hold-proc`）。
- **颜色**（`color_<页面>`，六位十六进制）：除画板外每种模式可以定一个颜色，留空用它自己的配色。
- **跑马灯**：文字由网页画成 8 行高的点阵发给设备（每列两位十六进制，最多 600 列），所以设备不需要
  字库，中文也能显示：英文数字用网页里的 5×7 点阵字，其它字符用浏览器字体画大再缩到 8×8（笔画多
  的字会糊）。存在应用目录的 `marquee.txt`。速度 `text_speed`（每列毫秒）。
- **小球**：有姿态传感器（`imu` 扩展）时往板子低的一边滚，没有时自己弹。`tilt` 是传感器相对矩阵的
  方向（0..3 为转 0 / 90 / 180 / 270 度，加 4 为左右镜像），这块板竖着放时 3 是对的。
- **画板**：左键按住拖动上色，右键点击或拖动擦除（右键菜单已屏蔽），触屏用“橡皮”；LED 同步显示
  （一次调用约 100 毫秒，画得快时只发最新的一幅），松开后保存到 `drawing.txt` 并切到画板模式。
  还有自选颜色、填满、清空、撤销。
- **网页**（设备网页的应用选项卡）：模式按钮、各模式颜色、跑马灯文字和速度、画板、小球方向、亮度。
- **屏幕**：虚拟屏上每秒画一次同样的 64 格，WebREPL 镜像里能看到。
- `imu` 在 meta.ini 里是 `optional_extensions`（构建工具新加的键）：没有这个扩展的固件也能装。
- 网页接口（`/api/app/call?fn=...`）：`get`、`mode value=`、`color page= value=`、`draw pixels=`、
  `save pixels=`、`text cols= text=`、`speed value=`、`tilt value=`、`brightness value=`。
- 写应用时碰到的：这套 Scheme 没有 `odd?`，`random` 还是保留项（应用里用了自己的线性同余）。

## 7. 姿态传感器：`imu` 扩展 1.0（API 1.21）

`main/imu.c`：QMI8658，I2C 400 kHz，±4 g、±512 °/s、112 Hz。启动时先按 SDA 11 / SCL 12 找，找不到
再把两个脚对调试一次，地址 0x6B 和 0x6A 都试。

| 过程 | 结果 |
|---|---|
| `(imu-accel)` | `(x y z)`，单位 mg（1000 = 1 g），静止时就是重力 |
| `(imu-gyro)` | `(x y z)`，单位 0.1 °/s |
| `(imu-temperature)` | 芯片温度，单位 0.1 °C（比室温高很多，板子自己发热） |

读不到时返回 `#f`。数值沿芯片自己的轴，没有换算到矩阵的方向：这块板竖着放时实测约
`(-956 94 -5)`，合力约 1 g。4 毫秒内的多次调用共用一次读取。没有这个传感器的板子上
`(api-has? "imu-accel")` 是 `#f`。
