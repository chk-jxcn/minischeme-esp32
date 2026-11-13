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
- 姿态传感器还没有接（I2C：SDA 11、SCL 12，地址 0x6B，来自社区资料，未验证）。

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
matrix_order = grb            ; LED 的颜色顺序：grb（默认）或 rgb
matrix_layout = progressive   ; 每行同方向（默认）或 serpentine（隔行反向）
matrix_rotate = 0             ; 0 / 90 / 180 / 270，顺时针
```

这几个键新建的 config.ini 里没有，要用时自己加。LED 的排列和颜色顺序没有找到权威资料（原理图
是图片），默认值是按普通 WS2812 矩阵猜的：如果红绿颠倒改 `matrix_order`，画面方向不对改
`matrix_rotate`，隔行左右颠倒改 `matrix_layout`。

## 5. 验证过的和没验证的

- 验证过（板子 COM62，192.168.2.153）：启动、WiFi、应用加载、8 个过程的返回值和报错、
  100 帧 239 毫秒。
- **没验证**：LED 实际亮的位置和颜色（我看不到板子）。测试图案：左上角连续三颗红、右上角一颗
  绿、左下角一颗蓝、右下角一颗白。
- 没做：姿态传感器、针对 8×8 的示例应用、蓝牙在这块板上的测试。
