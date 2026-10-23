# MiniScheme 设备 API

本目录是给应用作者（包括 AI agent）看的：写一个新应用需要的接口、版本规则和应用框架都在这里。

| 文件 | 内容 |
|---|---|
| [v1.9.0.md](v1.9.0.md) | **API 1.9.0 参考**（当前版本）：每个接口的原型、说明、返回值、样例，按组件分组。由 `tools/gen_api.py` 生成，样例都由测试实际运行验证。 |
| [v1.6.0.md](v1.6.0.md) | API 1.6.0 参考（旧版本）。 |
| [v1.5.0.md](v1.5.0.md) | API 1.5.0 参考（旧版本）。 |
| [v1.4.0.md](v1.4.0.md) | API 1.4.0 参考（旧版本）。 |
| [v1.3.0.md](v1.3.0.md) | API 1.3.0 参考（旧版本）。 |
| [v1.2.0.md](v1.2.0.md) | API 1.2.0 参考（旧版本）。 |
| [v1.1.0.md](v1.1.0.md) | API 1.1.0 参考（旧版本）。 |
| [v1.0.0.md](v1.0.0.md) | API 1.0.0 参考（旧版本记录）。 |
| [applib.md](applib.md) | **应用框架**（`apps/lib/applib.scm`、`apps/lib/infopages.scm`）：页面、代理数据、布局、主题色、轮播、硬件信息页。大部分应用都基于它。 |
| 本文 | 版本规则、应用如何声明 API、固件调用应用的钩子、应用结构和工作流程。 |

接口的唯一数据源是 `api/spec/*.py`；维护说明（新增接口、升级版本）见 [api/README.md](../../api/README.md)。参考文档的条目用英文，因为同一份文字也编进固件，用作运行时帮助 `(help 'name)`。

## 1. 版本规则

API 版本号为 `major.minor.patch`：

- **patch**：只修正错误或补充文档，接口不变。
- **minor**：新增接口或可选参数，向后兼容。同一 major 下的旧应用都能继续运行。新接口的 `since` 记为新的 minor 版本。实现一个预留接口也属于 minor 变更。
- **major**：删除接口或改变已有接口的行为，不兼容。

**兼容判断**：应用声明需要 `api = M.m`。固件提供 `M'.m'.p'` 时，只有满足 `M == M'` 且 `m <= m'` 才能运行；patch 不参与判断。

固件提供的版本可以从三个地方查到：

- `(api-version)` → `"1.9.0"`；
- `GET /api/info` 里的 `"api"` 字段（以及 `"api_extensions"`）；
- 硬件信息页“系统”页的“固件”一行。

每个 API 版本都有自己的参考文件 `v<版本>.md`。升级版本时旧文件保留不改，作为那个版本的接口记录。

## 2. 应用如何声明需要的 API

在 `apps/<id>/meta.ini` 的 `[app]` 段写：

```ini
api = 1.0                 ; 需要的设备 API（major.minor）
extensions = esp32 1.0    ; 可选：用到的扩展 API
```

- **构建时**（`tools/build_apps.py`）：扫描应用源码中调用的 API 名称（排除应用自己 `define` 的名字），按各接口的 `since` 求出最低需要的版本和扩展。
  - 没写 `api`：自动写进镜像的元数据；
  - 写得比实际需要的低：构建报错；
  - 用到预留接口：给出警告，提示用 `(api-has? 'name)` 判断。
  - 构建输出示例：`calendar: API 1.0 (140 procedures)`。
- **安装时**（`tools/install_apps.py`）：读取板子 `/api/info` 的版本，不兼容的应用拒绝安装，并提示升级固件。
- **启动时**：固件和 WASM 模拟器在构建 ROM 镜像之前检查元数据。不兼容时屏幕显示“应用与固件不兼容”和原因（例如 `app needs API 1.1, the firmware provides 1.0.0`），5 秒后进入系统设置。设置页开机时同样显示这个原因。
- 没有 `api` 键的旧镜像视为需要 1.0。

## 3. 接口的组织

- **组件**：每个接口属于一个组件（`gpio`、`gui`、`wifi`、`fs`…），组件再归入几大类：Language、System、Hardware、Display、Network、Storage、Tools、Extensions、Host。
- **命名**：设备接口按 `component-function` 命名，例如 `gpio-write`、`time-ms`、`mem-free`、`led-set`、`sys-restart`。语言核心沿用标准 Scheme 名称（`car`、`+`、`string-append`）。
- **旧名**：原来 Arduino 风格的名字（`digital-write`、`millis`、`free-heap`、`ws2812-set`、`restart`…）保留为别名，在 major 1 内一直可用，调用的是同一个过程；新应用使用新名称。对照表见参考文件末尾的 “Older names”。
- **状态**：
  - **stable**：已实现。
  - **reserved**（预留）：名称和原型已经固定，还没有实现。包括 I2C、SPI、UART、定时器、蜂鸣器、HTTP、MQTT、键值存储、JSON、编码、随机数和一批字符串、列表工具。调用预留接口会报错 `unbound variable: i2c-open (reserved API since 1.0.0: not provided by this firmware, API 1.0.0)`；`(api-has? 'i2c-open)` 返回 `#f`。等它在某个 minor 版本中实现后，可以这样写兼容代码：

    ```scheme
    (if (api-has? 'http-get)
        (http-get "http://example.com/x.json")
        (json-feed-start ...))
    ```
- **适用范围**：
  - 大多数接口在固件、WASM 模拟器和 PC REPL 中都有；
  - 标为 **device only** 的只在固件和模拟器中有，例如 `config-get`、`app-list`、`fs-list`；
  - 标为 **PC REPL only** 的只在电脑上的 REPL 中有。

## 4. 扩展 API

某块硬件独有的功能做成**扩展 API**：名称以扩展名开头，单独编版本，只有具备该硬件的设备才提供。

- 目前只有 `esp32` 1.0.0：
  - 已实现：`esp32-mac`、`esp32-reset-reason`、`esp32-idf-version`；
  - 预留：`esp32-touch-read`、`esp32-psram-size`。
- 固件和 WASM 模拟器提供这个扩展，PC REPL 不提供。
- 运行时可以用 `(api-extensions)` 查询，返回 `(("esp32" . "1.0.0"))`。
- 应用用到扩展时，在 meta.ini 写 `extensions = esp32 1.0`（构建时会自动补上）。设备上没有这个扩展，或扩展版本不兼容时，应用不会启动。
- 在没有该扩展的设备上调用扩展接口，报错为 `unbound variable: esp32-mac (esp32 extension API: not on this device)`。

## 5. 运行时查询（`api` 组件）

| 调用 | 结果 |
|---|---|
| `(api-version)` | `"1.9.0"` |
| `(api-compatible? "1.0")` | `#t`：需要该版本的应用能在本固件上运行 |
| `(api-has? 'gpio-write)` | 本设备是否提供该接口（已实现、扩展存在、确实可调用） |
| `(api-info 'gpio-write)` | `("gpio" "1.0.0" stable #f all)`：组件、引入版本、状态、扩展、适用范围 |
| `(api-list 'adc)` | 某个组件（不写则全部）的接口名 |
| `(api-components)`、`(api-extensions)` | 组件列表；本设备的扩展及版本 |

## 6. 应用结构

```
apps/<id>/meta.ini      元数据、设置项说明、默认设置
apps/<id>/<id>.scm      程序（通常基于 applib：前面会拼上 apps/lib/applib.scm 和 infopages.scm）
```

设备上应用目录里还可以有 `schedule.ini`（定时唤起，见第 7 节），由应用或它的网页写入。

**meta.ini**：

- `[app]` 段：`id`、`name`（中文显示名）、`description`、`version`、`pages`、`api`、`extensions`；
- `[fields]` 段：设置页如何显示每个设置项，格式为 `类型 | 标签 | 说明`；
- `[config]` 段：设置项的默认值。

**设置的读取**：应用第一次启动时，固件用 `[config]` 生成应用目录里的 `config.ini`（`/spiffs/apps/<id>/config.ini`）；应用更新后，新增的键会自动补进已有的文件。应用用 `(app-config "key" default)`（applib）或 `(config-get "key")` 读取设置，应用自己的 `config.ini` 优先于系统的 `config.ini`；系统配置中的密码、WiFi 等敏感项应用读不到。

**应用目录和权限**：每个应用一个目录，市场安装的在 `/spiffs/apps/<id>/`，用户上传的在 `/spiffs/user/<id>/`。应用只能读写自己的目录，不能调用改系统的接口（`config-set!`、WiFi、字库写入、重启等）。详见 [applib.md 第 9 节](applib.md#9-app-folders-and-the-sandbox)。

**工作流程**：

```bash
python tools/build_apps.py <id>                        # 编译成 web_repl/apps/<id>.msbc，元数据附在镜像末尾
python tools/install_apps.py <板子IP> <id> --boot <id>  # 安装、设为开机应用并重启
```

新应用要加入 `tools/build_apps.py` 的应用表。需要的数据放在 PC 上的代理 `tools/app_proxy.py`（端口 8087）中转，格式为 `{"items":[{"t":类型,"a":…,"e":…}]}`，设备用 JSON feed 拉取。也可以先在 WebREPL 的 WASM 模拟器中运行验证，它与固件使用同一套 C 运行时。

## 7. 固件调用应用的钩子

应用（applib 已经全部实现）定义下列全局过程，固件在相应时机调用它们。没有定义的钩子跳过。

| 钩子 | 调用时机与参数 |
|---|---|
| `(app-tick hm sec week date lux backlight tick lunar)` | 每秒一次。`hm` 为 `"20:29"`，`sec` 为 `":06"`，`week` 为 `"周六"`，`date` 为 `"10月10日"`，`lunar` 为 `"农历九月初一"`；时钟未同步时是 `"--:--"` 等占位符，`lunar` 为 `""`。`lux` 是光照，`backlight` 是背光 0..255。时钟同步后，`tick` 是 **UTC 当天的秒数**（同步前是开机秒数）。 |
| `(app-step)` | 主循环每轮调用，用来取 JSON feed 的新数据等。 |
| `(app-button)` | 短按（按下少于 0.8 秒）。硬件信息页打开时由信息页处理，不调用。 |
| `(app-long-press)` | 短长按（默认 0.8 秒以上）。硬件信息页打开时由信息页处理（关闭信息页），不调用。 |
| `(app-mid-press)` | 中长按（默认 3.5 秒以上）。硬件信息页打开时不调用（这时中长按进入系统设置）。 |
| `(app-touch x y)` | 触摸（有触摸屏的设备）。 |
| `(app-resume)` | 从 FASL 堆快照恢复后调用，用来重新启动 C 侧的服务（例如 JSON feed）。 |
| `(app-schedule at)` | API 1.7.0。应用正在运行时到了它 `schedule.ini` 里的时间点，`at` 为 `"08:00"`。处理完后调用 `(app-return)`。 |
| `gui-render-proc` | 全局变量，值为无参的绘制过程。渲染器需要重绘时调用它；条带模式下每帧只执行一次，命令记录成显示列表后分条带回放。状态变化后用 `(gui-invalidate x y w h)` 标出需要重绘的行。绘制过程必须是确定的（同样的状态画出同样的内容）。 |
| `info-*` | 保留给共享的硬件信息页模块（`apps/lib/infopages.scm`）。 |

**按键手势**：短按、短长按（0.8 秒）和中长按（3.5 秒）交给应用（`app-button`、`app-long-press`、`app-mid-press`）；长长按（8 秒）打开硬件信息页，在信息页上短按翻页、短长按返回应用、中长按进入系统设置（这些由固件处理，应用收不到）。三个时间可以在 config.ini `[system]` 的 `button_long_ms`、`button_mid_ms`、`button_hold_ms` 中设定（设备网页“系统配置”的常用设置里也能改，保存后立即生效）。按住时右下角最多显示三条进度条。系统设置里没有长长按：短按切换下一项，短长按确认，中长按返回上一级（在主菜单上中长按退出设置），菜单里不再有【返回】项。

**定时唤起**（API 1.7.0）：应用目录（`/spiffs/apps/<id>/` 或 `/spiffs/user/<id>/`）里有 `schedule.ini` 时，系统按其中的时间点启动这个应用：

```ini
[schedule]
times = 08:00, 12:30, 19:00   ; 本地时间，逗号分隔
return_after = 0              ; 可选：启动后多少秒自动返回，0 或不写表示等应用调用 (app-return)
persist = 1                   ; 可选（固件 1.0.68+）：重启、崩溃、断电后继续这次启动，直到 (app-return)
```

- 时钟同步后，每分钟检查一次所有已安装应用的 `schedule.ini`；系统设置运行时不检查。重启错过的时间点最多补两分钟。
- 到点的应用不是当前应用：板子重启进入它，`(app-scheduled)` 返回 `"08:00"`。应用做完后调用 `(app-return)`，板子重启回到开机应用（`[system] boot_app`，即原来的应用）。
- 到点的应用就是当前应用：不重启，调用它的 `(app-schedule at)` 钩子，此后 `(app-scheduled)` 也返回该时间；没有这个钩子则什么都不做。
- 同一时间有多个应用到点：依次运行，前一个 `(app-return)` 后启动下一个，最后一个返回开机应用。一个定时应用运行期间又有别的应用到点，也排在它后面。
- 定时应用崩溃或板子重启（按键进入设置等）后，回到开机应用，排队的启动取消。`persist = 1` 的应用例外：启动和排队记在 `/spiffs/.sched_launch`，重启、崩溃、断电（进入设置模式那一次除外）后再次启动它，直到它调用 `(app-return)`；连续 3 次启动都没能运行满 1 分钟就放弃。
- 应用自己（`write-file-string`）或它的网页（`/api/app/file?name=schedule.ini`）写 `schedule.ini`；吃药提醒应用是完整的例子。

**出错处理**：以下任一情况固件判定应用崩溃，显示错误原因后进入设置：

- 10 秒内钩子出错 10 次；
- 连续 3 帧渲染失败；
- 内存耗尽。

## 8. 资源约束

- 板子：ESP32-D0WDQ6，320×480 ILI9488，RGB565 颜色值（用 `(gui-rgb r g b)` 生成）。
- 应用 VM 堆约 88 KB，应用常驻堆应低于约 32 KB。
- 字节码和常量从 Flash 的代码 ROM 中原地执行（XIP），不占 RAM。
- 中文字体来自系统字库分区，见 `font-use`、`fontrom-list`。
- 设备上没有 `init.scm` 库：只能使用参考文件中列出的接口。PC REPL 独有的过程（如 `print`）在设备上不存在。

## 9. 版本记录

| 版本 | 日期 | 内容 |
|---|---|---|
| 1.0.0 | 2026-10-10 | 第一个带版本号的 API：现有接口按组件整理，统一为 component-function 名称（旧名保留为别名）；加入 `api-*` 查询接口、预留接口和 `esp32` 扩展 1.0.0。 |
| 1.1.0 | 2026-10-11 | 新增 `market` 组件（仅设备）：`market-refresh`、`market-status`、`market-apps`、`market-install`、`market-cancel`。在板子上只有系统设置里可以安装和更新应用，运行应用时只能读取列表。 |
| 1.2.0 | 2026-10-12 | JSON feed 去掉所有固定上限：键的个数和长度、选择器、行数、值文本、单个值长度、嵌套深度、URL 长度、`json-feed-ints` 的个数都按需动态分配，只受内存限制（板子上为系统保留 16 KB 空闲堆）。pool-bytes、value-len、depth 改为应用自选的上限，不传即不限。`json-feed-field` / `json-feed-ints` 可以用键名取值，`json-feed-status` 新增 `memory`。用到 6 个以上键的应用需要声明 `api = 1.2`。 |
| 1.3.0 | 2026-10-12 | 系统设置里也有中长按：固件在设置模式下把中长按交给设置应用（`app-mid-press`，用作返回上一级），只关掉长长按。设置应用去掉了【返回】菜单项，因此声明 `api = 1.3`，不会装到旧固件上。 |
| 1.4.0 | 2026-10-12 | 新增 `gui-bitmap`：1 位点阵（二维码、图标）不论多少点都只占一条显示列表命令，点阵数据放在字符串池里；以前用 `gui-rect` 逐段画，一个二维码就要约 350 条命令（上限 512）。 |
| 1.5.0 | 2026-10-14 | 新增 `gui-image`、`image-info`：从文件绘制图片（baseline JPEG，以及由 GIF 转换来的 MANI 动图，按帧间隔播放，速度可调）。每个条带只解码它覆盖的 JPEG MCU 行，不需要帧缓冲，打开一张图约占 6 KB。应用网页可以经 `/api/app/file` 读写自己目录里的文件；应用的 meta.ini 可以用 `storage_kb` 申请更大的存储空间。电子相册应用声明 `api = 1.5`。 |
| 1.6.0 | 2026-10-20 | 合并应用：meta.ini `[combo] members = a, b` 列出它带上的应用，`(config-get key app-id)` 读取成员应用自己的 config.ini，新增 `(app-has-config? app-id)`。旧固件忽略 app-id，合集应用用 `(defined? 'app-has-config?)` 兼容。 |
| 1.7.0 | 2026-10-22 | 定时唤起：应用目录里的 `schedule.ini` 列出时间点，系统到点启动应用（正在运行则调用 `app-schedule` 钩子）；新增 `(app-scheduled)`、`(app-return)`。新增 `(gui-text-scaled x y str color scale [font-id])`：用现有字体按整数倍放大绘制，不需要单独的大字体。预留的 `(time-local)` 实现为正式接口（仅设备）：本地日期时间 `(年 月 日 时 分 秒 星期)`，用于取年份或按天计数。吃药提醒应用声明 `api = 1.7`。 |
| 1.8.0 | 2026-10-23 | 配色方案：新增 `theme` 组件的 `(theme-css)`，任何应用都可以只读系统的配色文件 `/spiffs/theme.css`。应用框架（gui.scm）按别名取颜色（函数给的颜色 → 应用目录的 theme.css → 系统的 theme.css → 应用的默认值 → 内置白天 / 黑夜配色，都没有时用别名最后一段的大类），按光感、固定时间或日出日落在白天和黑夜间自动切换，见 [applib.md](applib.md) 4.8。用到框架的应用都声明 `api = 1.8`。 |
| 1.9.0 | 2026-10-23 | 应用镜像格式 MSBC 第 2 版：文件头之后是符号表，每个名字只存一次，常量里按序号引用（`MSBC_CONST_SYMREF`），镜像小约四分之一；第 1 版照样能读。没有新接口；编译器默认写第 2 版，所以应用声明 `api = 1.9`（`build_apps.py --msbc1` 写第 1 版，给旧固件用）。 |
