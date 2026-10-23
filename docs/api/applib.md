# MiniScheme app framework (applib)

How to write an app for the 320x480 (portrait) ESP32 display board on the shared
app framework. Read it together with:

- [v1.0.0.md](v1.0.0.md): every device procedure (`gui-*`, `font-*`, `json-feed-*`,
  `config-get`, ...), generated from `api/spec`.
- [README.md](README.md): API versions, the firmware hooks, resource limits.

The framework is two Scheme files that `tools/build_apps.py` compiles **into each
app image**, after `web_repl/gui.scm`:

| File | What it gives |
|---|---|
| `web_repl/gui.scm` | Theme colors (`theme-*`, `col-*`), font ids `FONT-SYS-16` / `FONT-CLOCK-32`, a few widgets. |
| `apps/lib/infopages.scm` | The hardware info pages (memory, storage, network, system) that a long press opens over any app. |
| `apps/lib/applib.scm` | Pages, data from the app proxy, header/footer, layout and text helpers, charts, carousel, clock fields, and every firmware hook. |

There is no `init.scm` library on the device: an app may use only the procedures
in v1.0.0.md, the definitions of these three files, and its own definitions.

Contents:

1. [Quick start](#1-quick-start)
2. [meta.ini and settings](#2-metaini-and-settings)
3. [Page model and data flow](#3-page-model-and-data-flow)
4. [Reference](#4-reference)
5. [Layout and style conventions](#5-layout-and-style-conventions)
6. [Writing a proxy endpoint](#6-writing-a-proxy-endpoint)
7. [Pitfalls](#7-pitfalls)
8. [Local pages and the app's own web page](#8-local-pages-and-the-apps-own-web-page)
9. [App folders and the sandbox](#9-app-folders-and-the-sandbox)

---

## 1. Quick start

### What applib does for an app

An app declares a title and a list of pages. Each page has a name, a procedure
that builds its data URL, a refresh interval, and a render procedure. applib then:

- fetches each page's URL from the app proxy with the JSON feed, in the background,
  and keeps the last documents so a page switch shows data at once;
- draws the header (title, subtitle or page name, page number, clock, carousel
  countdown) and the footer (data time, fetch state, errors, size and latency);
- draws a "waiting" screen until a page has data;
- turns pages on a short press (and touch in the simulator), optionally by itself
  (carousel);
- provides the hooks the firmware calls: `app-tick`, `app-step`, `app-button`,
  `app-touch`, `app-resume`, and the global `gui-render-proc`;
- loads a 16 px CJK font into slot 2 (`F-TEXT`) for all text.

The short, the long (0.8 s) and the mid press (3.5 s) are the app's; holding 8 s opens
the hardware pages (`infopages.scm`), where a mid press opens the system settings
(firmware). The times are config.ini [system] `button_long_ms`, `button_mid_ms`,
`button_hold_ms`.

### Directory layout

```
apps/<id>/<id>.scm     the program (applib, infopages and gui.scm are put in front of it)
apps/<id>/meta.ini     metadata, settings form, default settings
```

Use the same `<id>` for the directory, the `.scm` file and `[app] id`: the build
names the image `<id>.msbc`, and the board names the settings file
its folder `/spiffs/apps/<id>/` (image `app.msbc`, settings `config.ini`) after `[app] id`.

### A one-page app

This app uses the proxy's existing `/app/ping` endpoint, which answers
`{"items":[{"t":"ok","a":"HH:MM"}]}`, so it runs without proxy changes.

`apps/ping/ping.scm`:

```scheme
;;; =========================================================================
;;; Proxy check, 320x480 portrait, on the app framework (applib.scm).
;;; Data: the app proxy's /app/ping: {"items":[{"t":"ok","a":"HH:MM"}]}.
;;; =========================================================================

(define (ping-url) (app-url "/app/ping"))

(define (ping-render items)
  (let ((ok (items-find items "ok"))          ; the type is a string
        (cx (quotient (scr-w) 2))
        (y (+ (body-top) 60)))
    (if ok
        (begin
          (txt-center cx y "代理连接正常" theme-success)
          (txt-center cx (+ y 40) "代理的时间" theme-text-muted)
          (big (- cx (quotient (big-w (it-a ok)) 2)) (+ y 64) (it-a ok) theme-text-main))
        (txt-center cx y "代理没有返回 ok" theme-warning))
    ;; app-feed-url is a plain global: no file access while drawing
    (txt-fit 10 (- (body-bottom) 20) (- (scr-w) 20) app-feed-url theme-text-muted)))

(app-define! "代理测试"
  (list (app-page "首页" ping-url 30000 ping-render)))
```

`apps/ping/meta.ini`:

```ini
[app]
id = ping
name = 代理测试
description = 检查应用代理是否可用
version = 1
pages = 1
api = 1.0 ; the device API it needs (docs/api)

[config]
; no settings
```

### A two-page app

Both pages read one document (`/app/hello`, written in
[section 6](#6-writing-a-proxy-endpoint)): pages with the same URL share one fetch.

`apps/hello/hello.scm`:

```scheme
;;; =========================================================================
;;; Hello, 320x480 portrait, on the app framework (applib.scm).
;;; Data: the app proxy's /app/hello?name=...
;;;   greet  a: greeting  b: day of the year  c: the name
;;;   row    a: label     b: signed change ("+1.25")
;;;   series a: comma-separated integers
;;;   1 home: the greeting   2 a list and a chart
;;; =========================================================================

(define HELLO-INTERVAL 60000)            ; the proxy caches 60 s

;; Called once, by app-define!: settings are read at the start
(define (hello-url)
  (app-url (string-append "/app/hello?name=" (url-encode (app-config "name" "World")))))

(define (hello-render-home items)
  (let ((g (items-find items "greet")) (cx (quotient (scr-w) 2)) (y (+ (body-top) 40)))
    (if (not g)
        (txt 14 y "没有问候数据" theme-text-muted)
        (let* ((n (it-b g))                    ; digits only: the big font has no CJK
               (pre "今年第 ") (post " 天")
               (w (+ (txt-w pre) (big-w n) (txt-w post)))
               (x (- cx (quotient w 2))))
          (txt-center cx y (it-a g) theme-text-main)
          (txt x (+ y 70) pre theme-text-muted)
          (big (+ x (txt-w pre)) (+ y 62) n theme-accent)
          (txt (+ x (txt-w pre) (big-w n)) (+ y 70) post theme-text-muted)))))

(define (hello-render-list items)
  (section (body-top) "列表")
  (let loop ((rows (items-all items "row")) (y (+ (body-top) 26)) (i 0))
    (if (and (pair? rows) (< i 8))
        (let ((r (car rows)))
          (txt 10 y (it-a r) theme-text-main)
          (txt-right 310 y (it-b r) (rise-color (it-fixed r 2 2)))   ; "+1.25" -> 125
          (gui-line 10 (+ y 21) 310 (+ y 21) theme-divider)
          (loop (cdr rows) (+ y 26) (+ i 1)))))
  (let ((s (items-find items "series")))
    (if s
        (let* ((vals (it-ints s 1)) (r (series-range vals 10)))
          (section 260 "走势")
          (gui-rect 10 286 300 120 theme-card-hdr #t)
          (line-chart 10 286 300 120 vals (car r) (cdr r) theme-accent)))))

;; Page 1's header: the title, then this text in the accent color
(set! app-subtitle-proc
      (lambda (items) (let ((g (items-find items "greet"))) (if g (it-c g) #f))))

;; Last: app-define! calls the URL procedures and starts the first fetch
(app-define! "问候"
  (list (app-page "首页" hello-url HELLO-INTERVAL hello-render-home)
        (app-page "列表" hello-url HELLO-INTERVAL hello-render-list)))
```

`apps/hello/meta.ini`:

```ini
[app]
id = hello
name = 问候
description = 示例：代理的问候、列表和走势
version = 1
pages = 2
api = 1.0 ; the device API it needs (docs/api): runs on firmware API 1.x, x >= 0

[fields]
; how the settings page shows each [config] key: type | label | help
; types: text, number:min-max, bool, select:value=label,value=label
; (no ASCII ; or # in these lines: they start a comment)
name = text | 名字 | 问候的对象，如 世界
carousel = bool | 自动翻页 | 打开后按下面的时长依次显示各页
carousel_seconds = text | 每页秒数 | 一个值，或每页一个，如 15,10（后面的页沿用最后一个值）

[config]
name = World ; who to greet
carousel = 0 ; 1: turn the pages by themselves
carousel_seconds = 10 ; seconds per page, or one per page: 15,10 (the last repeats)
```

### Build

`tools/build_apps.py` builds only the apps in its `APPS` table. Apps on applib are
listed in one tuple; add the new id there:

```python
# tools/build_apps.py
for _app in ('weather', 'news', 'stocks', 'usage', 'calendar', 'hello'):
    if os.path.exists(os.path.join(ROOT, 'apps', _app, f'{_app}.scm')):
        APPS[_app] = (APP_LIB + [f'apps/{_app}/{_app}.scm'], [], None)
```

Then:

```bash
python tools/build_apps.py hello          # one app; no names: every app
```

The source is `gui.scm + infopages.scm + applib.scm + hello.scm`, compiled with the
WebREPL's compiler. Output: `web_repl/apps/hello.msbc` with `meta.ini` appended
(and `web_repl/apps/index.json` updated). The build prints the API the code needs
(`hello: API 1.0 (N procedures)`), fills in `[app] api` when missing, fails when
`api` is declared lower than needed, and warns about reserved (unimplemented) APIs.

Fonts: each literal `(use-font ID "face" [SIZE] ['embed|'system])` form is replaced
by an embedded subset of that face holding ASCII plus the characters of the app's
string literals, unless the form ends in `'system` (or the build runs with
`--system-fonts`). See [section 5](#fonts).

For regression coverage, `tests/test_apps.c` runs each app against a fixture
document in `tests/data/apps/` (the `DOCS` table maps URL substrings to files) and
checks, among other things, that the live heap stays under 32 KB.

### Install on the board

```bash
python tools/install_apps.py <board-ip | COMn> hello              # upload /spiffs/apps/hello/app.msbc
python tools/install_apps.py <board-ip> hello --boot hello        # also make it the boot app and restart
python tools/install_apps.py <board-ip> --list                    # apps on the board
```

An app needing a newer API than the board's firmware is refused. On the board, the
boot app can also be chosen in the system settings ("启动应用"; KEY 18 held 8 s, then a
mid press on the hardware pages), or on the
board's settings web page.

The board fetches data from `tools/app_proxy.py` running on a PC:

```bash
python tools/app_proxy.py [--port 8087]
```

and `config.ini [app] proxy = http://<pc-ip>:8087` on the board (also settable on its
settings page).

### Test in the WebREPL simulator

```bash
cd minischeme-webrepl && python server.py --port 8087    # http://localhost:8087
```

Open the page, click "📦 App 管理", install the app from the list of built apps
(`web_repl/apps/index.json`), then "设为启动并重启". The simulator runs the same C
runtime as the firmware (band rendering, JSON feed, fonts, clock tick). `server.py`
answers any `http://.../app/...` URL itself by importing `tools/app_proxy.py`, so no
proxy process is needed; restart `server.py` after editing `app_proxy.py`. The "KEY
18" button gives short and long presses; a click on the screen calls `app-touch`.
The app manager's form edits the app's settings (see below).

From the command line (agents, scripts), `tools/sim.py` in minischeme-webrepl runs the same
page headless: `python tools/sim.py --app my_app.scm --wait 3 --press short --shot out.png`
(see its `--help`).

---

## 2. meta.ini and settings

`meta.ini` is appended to the image as a trailer
(`[meta][u32 length][u32 FNV-1a]["MSMD"]`); older firmware ignores it.

### `[app]`

| Key | Meaning |
|---|---|
| `id` | App id: names the app's folder `/spiffs/apps/<id>/` (1..31 of `A-Z a-z 0-9 _ -`; without a valid one the folder name is used). Keep it equal to the directory name. |
| `name` | Display name (Chinese is fine): app lists, settings page, the system info page. |
| `description` | One line for app lists. |
| `version` | Shown as `v<version>` in app lists and on the "系统" info page. |
| `pages` | Page count, for app lists only (not checked against the code). |
| `api` | Device API the code needs, `major.minor` (`1.0`). Added by the build when missing; the build fails when it is lower than the code needs; install and boot refuse an incompatible firmware. |
| `extensions` | Extension APIs used, e.g. `esp32 1.0` (comma-separated). Added by the build when missing. |

### `[fields]`: the settings form

One line per `[config]` key: `key = type | label | help`. The form shows the keys in
`[config]` order; a key without a `[fields]` line is a text box labelled with the key,
with its `[config]` comment as help.

| Type | Form control | Stored value |
|---|---|---|
| `text` | text box | the text |
| `number`, `number:min-max` | number box with limits | digits |
| `bool` | checkbox | `1` or `0` |
| `select:v1=label1,v2=label2` | drop-down | the value `vN` (labels may not contain `,` or `=`) |

**Comment caveat:** in every line of meta.ini and of the settings ini, an ASCII `;`
or `#` starts a comment. Labels, help texts and values must not contain them (use
full-width `；` in Chinese text). The same applies to `[config]` values (no `#` in a
URL or a color).

### `[config]`: defaults

`key = default ; comment`. On the app's first start the firmware (and the simulator)
writes `config.ini` in the app's folder from this section. When an updated app brings new
`[config]` keys, the next start appends just the missing keys with their defaults;
existing values are kept.

Do not put secrets in the defaults: the meta trailer travels inside the image.

### How the app reads settings

- `(config-get "key")` (device API) returns the value from the app's `config.ini`,
  else from `/spiffs/config.ini`, as a string, or `#f`. The key is looked up
  regardless of `[section]`; values are cut at 159 bytes.
- `(app-config "key" default)` (applib) returns that string, or `default` when the
  key is missing **or empty**.
- `(app-config-set! "key" "value")` (device API) writes the app's own ini.
- Settings are edited in the WebREPL app manager, on the board's settings web page,
  or by editing the ini file. They take effect when the app restarts: the URL
  procedures run once, in `app-define!`.
- Every `app-config` call reads the ini files. Call it in URL procedures or at the
  top level, not in render procedures or every tick.

applib itself reads `proxy` (no default: unset, the footer says where to set it), `carousel` and
`carousel_seconds`, from the app's ini or `config.ini`.

An app's own server (`proxy_url`, `converter_url`) has two more keys in its `config.ini`, edited
on the configuration pages next to the address (apps never read them):

| Key | Meaning |
|---|---|
| `proxy_url_password` | The app service password for that server. Every request is plain HTTP and carries `X-MiniScheme-Device` (the board's MAC, the simulator's `SIM-...` id); with a password also `X-MiniScheme-Password` (base64 of it, firmware 1.0.69+; before, an encrypted channel registered with it). minischeme-proxy registers the device with the first password it sees and refuses another one (403). Base64 is no encryption: use a password of its own. A simple server of your own may ignore both headers. |
| `proxy_url_relay` | `1`: without a password, requests go through the system protocol converter (`<converter>/relay?url=`); the converter passes the device id on. Ignored when the server has a password (it is its own converter). |

The same pairs exist for `converter_url` (`converter_url_password`, `converter_url_relay`).

Since API 1.6.0, `(config-get "key" app-id)` reads another app's `config.ini`, but only in a
combined app and only for its `[combo] members` (any other id is an error);
`(app-has-config? app-id)` tells whether that member has one. Older firmware ignores the
second argument of `config-get`: guard with `(defined? 'app-has-config?)`.

### Combined apps (合集, API 1.6)

A combined app shows pages of several apps in one carousel. Its meta.ini names the
member apps; the build (`tools/build_apps.py`, or the WebREPL's 合并应用) puts applib,
`apps/lib/combo.scm` and every member's code into one image:

```ini
[app]
id = combo
name = 合集
pages = 5

[combo]
members = weather, calendar, wxpusher, stocks

[config]
entries =            ; app.page, comma separated, in carousel order; empty: every page
carousel = 1
carousel_seconds = 10
```

- **English page names**: every app lists its pages in `[app] page_ids` (`page_ids = today,
  month, moon`); `front` is always the first page. An entry is `<app>.<page>`
  (`calendar.month`), `<app>.front`, a page number (`weather.2`, 1 first) or a view the app
  declares with `app-view!` (`wxpusher.news[0]`, the newest message).
- **Settings**: each member keeps reading its **own** `config.ini`: while a member's code runs,
  `app-config` calls `(config-get key member-id)`; a member that never ran alone (no
  `config.ini`, `(app-has-config? id)` is `#f`) uses its meta.ini defaults. Edit a member's
  settings in that app, not in the combined app.
- **Hooks**: a member's `app-subtitle-proc`, `app-tick-proc`, `app-data-proc`,
  `app-long-press`, views and empty pages are kept per member; `app-header-proc` and
  `app-page-title-only` are cleared (the combined app titles each page "app page").
- Short press: next entry. Long press: the entry app's own long press.
- The combined app needs API 1.6 (the build declares it); older firmware does not install it.

---

## 3. Page model and data flow

### Pages

```scheme
(app-page name url-proc interval-ms render-proc)   ; => #(name url-proc interval render)
(app-define! title (list page ...))
```

- `name`: shown in the header on pages 2..N (page 1 shows the subtitle instead).
- `url-proc`: `() -> string`, called **once** by `app-define!`. Build it with
  `(app-url "/app/<name>?...")` and `url-encode` for query values. Pages whose URLs
  are equal share one document and one fetch.
- `interval-ms`: how often the board refetches while that URL is active. Match it to
  the proxy's cache time; a failed fetch is retried after 5 s.
- `render-proc`: `(items) -> any`, draws the body between header and footer.
- `title`: the header title (`app-title`); may be shorter than `[app] name`.

`app-define!` stores the pages, evaluates every URL procedure, reads the carousel
settings (only with 2+ pages), loads the fonts, starts the feed for page 1 and
invalidates the screen. Put it last in the file; set `app-subtitle-proc`,
`app-tick-proc` and `app-render-empty-pages` before it.

Only one JSON feed runs at a time: the current page's URL. Turning to a page with
another URL starts its feed (fetched at once); the cached document of that URL is
shown until the new one arrives.

### Data: the items model

The proxy answers `{"items":[{"t":type,"a":..,"b":..,"c":..,"d":..,"e":..}, ...]}`.
applib reads each object into a vector `#(t a b c d e)`:

- slot 0, the type, is a **symbol** (`'day`);
- slots 1..5 (`a`..`e`) are **strings**, or `#f` for missing or empty values
  (numbers in the JSON arrive as their text).

The list of vectors is cached per URL with the device time of the fetch. Accessors:

| Call | Result |
|---|---|
| `(items-find items "day")` | first item of type `"day"`, or `#f` (type as a **string**) |
| `(items-all items "day")` | all items of that type, in document order |
| `(items-where items "idx" 4 "sh000001")` | first `"idx"` item whose field 4 (`d`) equals the string |
| `(it-a it)` .. `(it-e it)` | field text, `""` when missing |
| `(it-int it k)` | field k (1..5) as an integer, `#f` when missing or not numeric |
| `(it-fixed it k digits)` | `"12.34"` with 2 digits -> `1234`; `#f` when missing or not numeric |
| `(it-ints it k)` | `"3,-1,7"` -> `(3 -1 7)`; `'()` when missing |
| `(app-keys! name ...)` | more fields per item beyond t, a..e, by name (`(app-keys! "topic")`), before `app-define!`; needs API 1.2 |
| `(it-key it name)` | the field the app named with `app-keys!`, `""` when missing |
| `(it-t it)` | the type symbol |

Conventions used by the existing proxy endpoints: series (temperatures, prices, an
intraday trend) are one field of comma-separated integers, scaled to fixed point
(basis points, tenths); several small values share a field separated by commas and
are split with `str-part`; numbers meant for display are sent as preformatted text.

### States of a page

The render procedure is called only when the page's URL has a cached document:

| State | Body | Footer (left) |
|---|---|---|
| no document yet, fetching | "正在获取数据..." and the URL | "加载中..." |
| fetch failed (HTTP, network) | the same, plus a hint to run `app_proxy.py` | "连接失败: <error>" in the warning color |
| proxy answered an `error` item | (old data if any) | "代理: <message>" in the warning color |
| refetching after a page switch | render with the cached items | "刷新中..." |
| data | render with the items | "更新于 HH:MM" |

- `app-render-empty-pages`: a list of page **indexes** (0-based) whose render
  procedure is called with `'()` before any data, instead of the waiting screen (the
  calendar's page 1 shows its clock without a proxy). Such a render procedure must
  handle `'()`.
- A document whose **first** item is `{"t":"error","a":msg}` sets `app-error` and
  keeps the old data. The next good document clears it.
- The footer's time comes from an item `{"t":"upd","a":"HH:MM"}` when the document
  has one (the time of the data at its source); otherwise from the device clock at
  fetch time ("已更新" when the clock was not set). On the right, after a successful
  fetch: document size and fetch time ("2.1KB 182ms").

### Header

From the left: `app-title`; on page 1 the text of `(app-subtitle-proc items)` (when
the procedure is set, there are items, and it returns a non-empty string), on other
pages the page name, in `theme-accent`; with a carousel, the countdown bar; on pages
2..N the badge "n/N"; the clock `app-time` at the right edge. The title and subtitle
are cut or scrolled to fit (`txt-fit`).

- `app-header-proc` `(lambda (items x w) ...)` draws page 1's title area `[x, x + w)`
  itself (the calendar: the date and the weekday at a fixed offset).
- `app-page-title-only` `#t`: pages 2..N show the page name alone, without `app-title`
  (the calendar: 月历, 月相).
- `app-index-always` `#t`: the "n/N" badge also on page 1 (the combined app).

### Carousel

With `carousel = 1` (also `on`, `yes`, `true`) in the app's ini or `config.ini`, and
two or more pages, pages turn by themselves. `carousel_seconds` is one value or one
per page (`15,10,10`, the last repeats), each clamped to 3..3600. A bar left of the
page number counts down (2 px per second, 1 px for pages over 30 s, at most 90 px).
A short press still turns the page and restarts the count. The carousel stops while
the info pages are open and restarts its count when they open and close. From a
REPL: `(app-carousel-pause)`, `(app-carousel-resume)`, `(app-carousel-state)`. While
turning, the cache keeps every page's document.

### Clock fields

Set by `app-tick` every second, before `app-tick-proc` runs:

| Global | Value | Before the clock is synced | Before the first tick |
|---|---|---|---|
| `app-time` | `"20:29"` | `"--:--"` | `"--:--"` |
| `app-sec` | `":06"` (with the colon) | `":--"` | `"--"` |
| `app-week` | `"周六"` | `"未同步"` | `""` |
| `app-date` | `"10月10日"` | `"等待网络校时"` | `""` |
| `app-lunar` | `"农历九月初一"` | `""` | `""` |
| `app-utc-sec` | second of the UTC day, 0..86399 | seconds since boot | `0` |

`app-tick` invalidates the header when `app-time` changes and the footer every
second tick. It drops `lux` and `backlight`; use `light-lux`, `backlight-get` etc.
if needed. `(time-synced?)` tells whether the clock is set.

`app-tick-proc`, when set, is called as `(app-tick-proc tick)` every second (also
while the info pages are open). Use it for live parts of a page.

### Redrawing

Drawing happens only through `gui-render-proc` (= `app-render`), which the renderer
calls for rows marked dirty with `(gui-invalidate x y w h)`. Invalidation is by
**rows**: `x` and `w` are ignored and rows `y..y+h-1` are drawn again. On the board
(band mode) the render procedure runs once per frame with drawing recorded into a
display list, then replayed band by band; the clip covers only the dirty rows.

- New data, page switch, carousel: applib invalidates everything.
- Minute change: the header rows. Every 2 s: the footer rows.
- A live element (a seconds clock, world clocks): remember its y during render and
  invalidate only those rows from `app-tick-proc`; skip it while `(info-active?)`.
- Inside a render procedure, wrap expensive sections in `(if (gui-visible? y h) ...)`
  so a partial redraw skips them (applib does this for header, body and footer).

From the calendar (`apps/calendar/calendar.scm`):

```scheme
(define cal-clock-y 0)                   ; set by the page-1 render procedure

(set! app-tick-proc
      (lambda (tick)
        (if (and (= app-page-index 0) (not (info-active?)) (> cal-clock-y 0))
            (gui-invalidate 0 cal-clock-y (scr-w) 82))))
```

To refetch (e.g. a new day), call `(app-start-feed!)`: it starts the current page's
URL again at once (the URL itself is not rebuilt).

---

## 4. Reference

Everything below is a global in the app's image. applib has no private prefix:
names starting with `app-` are framework state and procedures, the short unprefixed
names (`txt`, `big`, `section`, `str-*`, `it-*`, `items-*`, `take`, `clamp`, ...)
are helpers, and all `info-*` names belong to the info pages. App definitions share
this namespace: prefix them with the app (`cal-`, `news-`, `hello-`) and never
redefine a framework name unless you mean to replace it.

Arguments: `x y w h` pixels, `c`/`color` an RGB565 integer, `s` a string, `it` an
item vector, `items` a list of item vectors, `k` a field index 1..5 (`a`..`e`).

### 4.1 Lifecycle and hooks

| Definition | Description | Returns |
|---|---|---|
| `(app-define! title pages)` | Installs the pages (see 3), reads carousel settings, loads fonts, starts the feed, invalidates. | unspecified |
| `(app-page name url-proc interval render)` | A page descriptor. | `#(name url-proc interval render)` |
| `gui-render-proc` | Set to `app-render`; the firmware's draw entry. The info pages swap it while open. | |
| `(app-render)` | `gui-clear` with `theme-bg`, `app-render-page`, `gui-flush`. | |
| `(app-step)` | Firmware hook, every main-loop pass: takes a new feed document when `json-feed-seq` changed. | |
| `(app-tick hm sec week date lux bl . rest)` | Firmware hook, every second; `rest` = `(tick lunar)`. Updates the clock fields, invalidates header/footer, runs the carousel and `app-tick-proc`. | |
| `(app-button)` | Firmware hook, short press: `(app-next-page!)`. Not called while the info pages are open. | |
| `(app-touch x y)` | Hook for a touch: `(app-button)`. Only the WASM simulator calls it (screen click). | |
| `(app-resume)` | Hook after a FASL snapshot restore: reloads fonts, restarts the feed, invalidates. | |

applib defines no `app-long-press` or `app-mid-press`: define them to use the long press
(0.8 s) and the mid press (3.5 s), which are the app's (the hardware pages open with an
8 s hold and take the presses while they show). To replace a hook, assign it after applib, e.g.
`(set! app-button (lambda () (app-goto! 0)))`; the firmware looks hooks up by name on
every call.

### 4.2 Pages and navigation

| Definition | Description | Returns |
|---|---|---|
| `app-title` | Header title (set by `app-define!`). | string |
| `app-subtitle-proc` | `#f` or `(lambda (items) ...)` giving the text after the title on page 1. | string or `#f` |
| `app-tick-proc` | `#f` or `(lambda (tick) ...)` called every second. | |
| `app-render-empty-pages` | Page indexes rendered with `'()` before data. Default `'()`. | list |
| `app-header-proc` | `#f` or `(lambda (items x w) ...)` drawing page 1's title area itself. | |
| `app-page-title-only` | `#t`: pages 2..N are titled by their name alone. Default `#f`. | boolean |
| `app-index-always` | `#t`: the "n/N" badge on page 1 too. Default `#f`. | boolean |
| `(app-view! name page proc)` | Declares a view a combined app may show: entry `<app>.<name>[i]` calls `(proc items i)` on page `page`'s data (wxpusher: `news`). | |
| `app-pages` | Vector of page descriptors. | vector |
| `app-urls` | Vector of the pages' URLs (built once). | vector |
| `app-page-index` | Current page, 0-based. | integer |
| `(app-npages)` | Number of pages. | integer |
| `(app-cur)` | Current page descriptor. | vector |
| `(app-goto! i)` | Shows page `i`: restarts the carousel count, starts the feed if the URL differs, invalidates. | |
| `(app-next-page!)` | Next page, wrapping to page 1. | |
| `(app-carousel-pause)` | Stops the carousel on this page (REPL). | `'off`, `'paused` or `'running` |
| `(app-carousel-resume)` | Starts counting this page again. | state symbol |
| `(app-carousel-state)` | `'off` (no carousel), `'paused` or `'running`. | symbol |
| `(app-carousel-reset!)` | Restarts the current page's countdown (no-op without carousel). | |
| `(app-page-seconds i)` | Carousel seconds of page `i`. Only valid while `app-carousel` is a list. | integer |
| `app-carousel` | Seconds per page (list) while turning, else `#f`. | |
| `app-carousel-left` | Seconds left on this page. | integer |
| `app-carousel-paused` | `#t` after `app-carousel-pause`. | boolean |

### 4.3 Data and settings

| Definition | Description | Returns |
|---|---|---|
| `(app-config key default)` | Setting `key` from the app's ini, else `config.ini`; `default` when missing or empty. Reads files on each call. | string or `default` |
| `(app-proxy)` | `proxy` setting without a trailing `/`; `""` when unset (then `app-start-feed!` fetches nothing and sets `app-error`). | string |
| `(app-url path)` | `(string-append (app-proxy) path)`. | string |
| `(app-items)` | Cached items of the current page's URL. | list, or `#f` before data |
| `(app-start-feed!)` | (Re)starts the JSON feed for the current page's URL; fetched at once. | |
| `(app-status key)` | A field of `(json-feed-status)`: `'state` (`idle` `connecting` `ok` `error`), `'error`, `'bytes`, `'fetch-ms`, `'fetches`, `'failures`, `'age-ms`, `'objects`, `'truncated`. | value or `#f` |
| `app-error` | Message of the last proxy `error` item, `#f` after a good document. | string or `#f` |
| `app-loading` | `#t` from a feed start until its first document. | boolean |
| `app-feed-url` | URL of the running feed. | string |
| `app-cache` | `(#(url items time) ...)`, newest first. | list |
| `(it-t it)` | Type symbol. | symbol |
| `(it-f it k)` | Field `k`, `""` when missing. | string |
| `(it-a it)` .. `(it-e it)` | `(it-f it 1)` .. `(it-f it 5)`. | string |
| `(it-int it k)` | `(it-fixed it k 0)`. | integer or `#f` |
| `(it-fixed it k digits)` | `string->fixed` of field `k`. | integer or `#f` |
| `(it-ints it k)` | `string->ints` of field `k`. | list (`'()` when missing) |
| `(items-find items type)` | First item whose type is the string `type`. | item or `#f` |
| `(items-all items type)` | All items of `type`. | list |
| `(items-where items type k value)` | First item of `type` whose field `k` is `equal?` to `value`. | item or `#f` |

### 4.4 Clock fields

`app-time`, `app-sec`, `app-week`, `app-date`, `app-lunar`, `app-utc-sec`: see the
table in [section 3](#clock-fields).

### 4.5 Layout and text

| Definition | Value / description | Returns |
|---|---|---|
| `F-TEXT` | `2`: the 16 px CJK text font slot. | |
| `F-BIG` | `FONT-CLOCK-32` (`1`): 32 px digits, only the characters `" %-./0123456789:AMP"`. | |
| `HDR-H` | `32`: header height. | |
| `FTR-H` | `24`: footer height. | |
| `LINE-H` | `20`: line pitch of wrapped text. | |
| `(body-top)` | `HDR-H + 6` = 38. | integer |
| `(body-bottom)` | `(gui-height) - FTR-H - 4` = 452 on 480 rows. | integer |
| `(scr-w)` | `(gui-width)` = 320. | integer |
| `(txt x y s c)` | Text in `F-TEXT`, top-left at x y. | |
| `(txt-w s)` | Width of `s` in `F-TEXT`. | integer |
| `(txt-right xr y s c)` | Right-aligned at `xr`. | |
| `(txt-center cx y s c)` | Centered on `cx`. | |
| `(txt-fit x y w s c)` | One line in `w` px; longer text scrolls or ends in "…" (`config.ini [text] marquee`). | |
| `(txt-wrap x y w h s c)` | Wrapped in a box with `LINE-H`; too tall text scrolls or is cut (`[text] wrap_marquee`). | |
| `(big x y s c)` | Text in `F-BIG`. | |
| `(big-w s)` | Width in `F-BIG`. | integer |
| `(big-right xr y s c)` | Right-aligned big text. | |
| `(big-unit x y num unit c)` | Big `num`, then `unit` in `F-TEXT` 2 px after it, 2 px lower. | |
| `(section y label)` | Label in `theme-accent` at x 10, then a `theme-divider` rule to the right edge. About 24 px tall. | |
| `(section-note y label note)` | The same with `note` right-aligned in `theme-text-muted`. | |
| `(app-init-fonts!)` | Loads slot 2: the system font when it is 16 px, else `msyh` 16, else `simsun` 16 from the font ROM. | |
| `(app-refresh-fonts!)` | Unloads and reloads slot 2, invalidates (after the default font changed). | |
| `(app-invalidate-all)` | Invalidates the whole screen. | |
| `(app-invalidate-body)` | Invalidates from `HDR-H` to the bottom (body and footer). | |

### 4.6 Drawing helpers

| Definition | Description | Returns |
|---|---|---|
| `(line-chart x y w h vals lo hi color)` | Polyline of integers scaled `lo..hi` (uses `gui-chart`); draws nothing for fewer than 2 values. | |
| `(bar-chart x y w h vals hi color)` | Bars of integers `0..hi`, bottom-aligned, pitch `w / n`, 1 px gaps. | the pitch (integer) |
| `(series-range vals min-span)` | Range with head room (1/8 of the span), at least `min-span` wide. | `(lo . hi)`; `(0 . min-span)` for `'()` |
| `(progress-bar x y w h pct color)` | Rounded track in `theme-dark-gray`, filled `pct` % (clamped 0..100, at least 6 px when non-zero). | |
| `(rise-color v)` | Chinese market colors: `v > 0` `theme-danger` (red), `v < 0` `theme-success` (green), `0` `theme-text-sub`, `#f` `theme-text-muted`. | color |

`web_repl/gui.scm` also provides `gui-box`, `gui-badge`, `gui-progress-bar`,
`gui-button`, `gui-stat-tile`, `gui-grid-cell`, `gui-content-box`,
`gui-text-align`, `gui-with-clip`, `gui-with-buffer`, `gui-draw-wifi-icon`,
`make-widget` / `gui-hit-test` / `gui-dispatch-touch`, `rgb`, and a few aliases.
The applib apps do not use them; `gui-badge`, `gui-button` and `gui-stat-tile` draw
their labels in font 0 (`FONT-SYS-16`), which has few CJK characters.

### 4.7 Strings and numbers

| Definition | Description | Returns |
|---|---|---|
| `(str-split s ch)` | `"a,b,,c"` `#\,` -> `("a" "b" "" "c")`. | list of strings |
| `(str-part s ch k)` | k-th (0-based) field, `""` when missing. | string |
| `(str-empty? s)` | `#t` for `#f` or `""`. | boolean |
| `(str-starts? s prefix)` | Prefix test. | boolean |
| `(url-encode s)` | Percent-encodes every byte except `0-9 A-Z a-z , - . _ ~`. | string |
| `(int->str n)` | `number->string`. | string |
| `(pad2 n)` | `5` -> `"05"`. | string |
| `(fixed->str v digits)` | `(fixed->str 1234 2)` -> `"12.34"`, `(fixed->str -5 1)` -> `"-0.5"`, `#f` -> `"--"`. | string |
| `(make-string-of ch n)` | `n` copies of `ch`. | string |
| `(clamp v lo hi)` | `v` limited to `lo..hi`. | number |
| `(list-min l)`, `(list-max l)` | Minimum / maximum; `l` must not be empty. | number |
| `(list-sum l)` | Sum; `0` for `'()`. | number |
| `(take l n)` | First `n` elements (fewer when short). | list |

### 4.8 Colors and color schemes (`apps/lib/gui.scm`, API 1.8)

Colors are RGB565 integers. The framework's colors are **aliases** resolved
through color schemes: a built-in **day** scheme (light) and **night** scheme
(dark, the colors every app had before), switched automatically.

`(theme-color c)` gives the color to draw with: a number passes as it is; a
string is a literal (`"#rrggbb"`, `"#rgb"`, `"rgb(r, g, b)"`) or an alias
(`"titlebar_bg"`) looked up in this order, the first found wins:

1. a color given to the call itself (a number or a literal);
2. the app's own `theme.css` (in its folder), the block of the scheme in use;
3. the system's `/spiffs/theme.css` (read with `(theme-css)`, API 1.8), the same block;
4. the app's defaults, `(theme-defaults! scheme alist)`;
5. the built-in day or night scheme.

An alias no step names falls back to its **category**, the part after the last
`_` (`titlebar_bg` → `bg`, `text_sub_fg` → `fg`, `card_border` → `border`),
looked up the same way. A value may name another alias, so users can define
their own (`my_gold: #ffcc33; accent_fg: my_gold;`).

`theme.css` (system or app, at most 16 KB) is CSS-like:

```css
/* comments */
day   { titlebar_bg: #d7e1ec; my_gold: #e0a000; warning_fg: my_gold; }
night { titlebar_bg: #152030; }
*     { seg_on_fg: #6fcf7f; }          /* every scheme */
sakura { bg: #fff0f5; }               /* a scheme of your own */
```

The mode is set in `config.ini [theme]` (an app's own config.ini may set the
same keys): `theme` = `auto` (default), `day` or `night`; with `auto`,
`theme_switch` = `light` (default: `light-lux` at or over `theme_light_day`,
120, gives day; at or under `theme_light_night`, 50, night; in between the mode
stays), `time` (`theme_day_start` 07:00 .. `theme_night_start` 19:00) or `sun`
(sunrise to sunset at `theme_lat` / `theme_lon`, the time zone taken from the
clock; the time when unknown). `theme_day` / `theme_night` name the scheme
each mode uses (`day` / `night`, or a block of theme.css). The device page's
**配色方案** tab edits all of it, with a preview, and the display output
(`color_bits`, `palette`), which applies after the scheme.

`applib.scm` calls `(theme-tick! utc-second)` once a second: every 10 s it
checks the mode and both theme.css files and, when anything changed,
re-resolves the colors and redraws the screen. An app without applib calls it
from its own `app-tick` (the settings and the PC monitor do).

The `theme-*` variables below are set from their aliases at the start and at
each change, so code that draws with them follows the scheme without change.
Code that keeps a color in its own variable at load time does not: use
`(theme-color "alias")` when drawing instead, and give the alias defaults:

```scheme
;; the album's surround: black at night, the day scheme's background by day
(theme-defaults! "night" '(("photo_bg" . "#000000")))
(theme-defaults! "day" '(("photo_bg" . "bg")))
(gui-clear (theme-color "photo_bg"))
```

| Variable | Alias | Night | Day | Use |
|---|---|---|---|---|
| `theme-bg` | `bg` | #0b1118 | #f3f5f8 | canvas; also text on filled color boxes |
| `theme-card-bg` | `card_bg` | `bg` | `bg` | cards (outlined) |
| `theme-card-border` | `card_border` | #5b7090 | #8d9cae | card outlines |
| `theme-card-hdr` | `titlebar_bg` | #152030 | #d7e1ec | header/footer strip, chart backgrounds |
| `theme-text-main` | `text_fg` | #e6ecf3 | #17202b | values, primary text |
| `theme-text-sub` | `text_sub_fg` | #bac7d6 | #33404f | secondary text |
| `theme-text-muted` | `text_muted_fg` | #a2b2c6 | #556377 | labels, hints, axis text |
| `theme-accent` | `accent_fg` | #62d0f0 | #0a6f9c | section labels, highlights |
| `theme-accent-dark` | `accent_bg` | #173f52 | #c2e0ee | page badge, selected-cell fill |
| `theme-success` | `success_fg` | #72d696 | #1d7f45 | good / falling price |
| `theme-warning` | `warning_fg` | #f0bc55 | #b8700a | caution, errors in the footer |
| `theme-danger` | `danger_fg` | #ee7c7c | #bf3434 | bad / rising price |
| `theme-purple` | `purple_fg` | #c0a2f2 | #6c49b0 | extra category color |
| `theme-dark-gray` | `inactive_fg` | #34414f | #c4cdd8 | inactive tracks |
| `theme-divider` | `divider_border` | #688098 | #a3b0bf | rules and grid lines |
| `theme-seg-on` | `seg_on_fg` | #6fcf7f | #1f7a35 | lit seven-segment digits |
| `theme-seg-off` | `seg_off_fg` | #142a1c | #dae6dd | unlit segments |
| | `bar_fg`, `bar_bg`, `icon_fg` | `accent_fg`, `titlebar_bg`, `accent_fg` | the same | progress bars, icons |
| | `fg`, `border` (categories) | #e6ecf3, #5b7090 | #17202b, #8d9cae | the default of any `*_fg` / `*_border` |

Apps add their own aliases with defaults: the weather app's `sun_icon_fg`,
`snow_icon_fg`, `thunder_icon_fg`, the album's `photo_bg`.

Plain palette (fixed, not part of a scheme): `col-black` 0, `col-white` 65535,
`col-red` 63488, `col-green` 2016, `col-blue` 31, `col-cyan` 2047,
`col-magenta` 63519, `col-yellow` 65504, `col-orange` 64800, `col-gray` 33808,
`col-dark-gray` 12710, `col-light-gray` 50712. Font ids: `FONT-SYS-16` 0,
`FONT-CLOCK-32` 1.

### 4.9 Framework internals

Used by applib itself; read or call them only as noted.

| Definition | Purpose |
|---|---|
| `APP-KEYS` (`APP-BASE-KEYS` `'("t" "a" "b" "c" "d" "e")` plus the app's `app-keys!`), `APP-SELECTORS` `'((0 prefix ""))` | JSON feed keys; every object with a `t` is kept. |
| `APP-POOL` 0, `APP-VALUE-LEN` 0 | No limit on value text per document or per value (the feed is bounded by memory only; API 1.2). |
| `APP-CACHE-MAX` 3 | Documents kept (all pages' while the carousel runs). |
| `CAROUSEL-BAR-H` 4, `CAROUSEL-BAR-COLOR` 46552 | Countdown bar height and color. |
| `app-last-seq` | Last `json-feed-seq` taken. |
| `(app-read-item i)` | Feed object `i` -> `#(type a b c d e ...)`, a slot per key of `APP-KEYS`. |
| `(app-take-document!)` | Copies a new document into the cache (or sets `app-error`), invalidates. |
| `(app-cache-entry url)`, `(app-cache-put! url items)`, `(app-cache-max)` | The per-URL cache; the home page's document is never dropped. |
| `(app-render-page)` | Header, body (render procedure / waiting screen) and footer, each only when `gui-visible?`. |
| `(app-render-header items)`, `(app-render-footer)`, `(app-render-waiting)` | The parts above. |
| `(app-header-sub items)` | Subtitle on page 1, page name elsewhere. |
| `(app-render-countdown xr)`, `(carousel-px secs)`, `(carousel-bar-w secs)` | The carousel bar. |
| `(app-carousel-config)`, `(app-carousel-tick!)` | Reads the carousel settings; one second of countdown. |

### 4.10 Info pages (`apps/lib/infopages.scm`)

KEY 18 held 8 s opens four pages over the app: 内存 (memory), 存储 (storage),
网络 (network), 系统 (system). A short press turns them, a long press closes them,
a mid press (3.5 s) opens the system settings. While open, `gui-render-proc` is `info-render`; the app keeps running (ticks,
feeds) but does not draw and gets no `app-button`.

| Definition | Description |
|---|---|
| `(info-active?)` | `#t` while the info pages show. Check it before invalidating from `app-tick-proc`. |
| `info-on-change` | `#f` or `(lambda (open?) ...)`, called when the pages open (`#t`) and close (`#f`). applib sets it to restart the carousel count; chain the old value if you replace it. |
| `(info-long-press)` | Firmware hook: opens or closes the pages. |
| `(info-button)` | Firmware hook while open: next info page. |
| `(info-tick hm . rest)` | Firmware hook every second: keeps `info-time`, redraws every 2 s while open. |
| `(info-open!)`, `(info-close!)` | Open / close programmatically. |
| `(info-render)` | Draws the current info page. |
| `info-page` | `0..3` while open, else `#f`. |
| `INFO-PAGE-NAMES` | `'("内存" "存储" "网络" "系统")`. |
| `INFO-F` | `7`: font slot loaded on open and **unloaded on close**. Apps must not use slot 7. |

The other `info-*` definitions are helpers of these pages (`info-row`, `info-bar`,
`info-divider`, `info-txt`, `info-bytes`, `info-duration`, `info-ref`, ...); they draw
in slot 7, so they work only while the pages are open. Do not define names starting
with `info-` in an app.

---

## 5. Layout and style conventions

### Screen

```
y   0 +--------------------------------+
      | title  subtitle     [bar] 2/4 12:30 |  header, HDR-H = 32, theme-card-hdr
   32 +--------------------------------+
   38 | (body-top)                      |
      |                                 |  body: x 10 .. 310
  452 | (body-bottom)                   |
  456 +--------------------------------+
      | 更新于 12:30          2.1KB 180ms |  footer, FTR-H = 24
  480 +--------------------------------+
```

- Margins: 10 px left and right (content 10..310); text in boxes 6..8 px in.
- Text rows: 16 px glyphs, 20..26 px pitch; two-line rows about 44 px.
- Sections: `(section y "label")`, content about 24..26 px below.
- Cards: `(gui-round-rect x y w h 6 theme-card-border #f)`, outlined, not filled.
  Two columns: `x = 10` and `x = 164`, width 146 (pitch 154).
- Chart areas: a `theme-card-hdr` filled rect, the chart on top; axis labels in
  `theme-text-muted`.
- Filled color boxes carry text in `theme-bg` (stocks' change box).
- Value colors: `theme-text-main` for values, `theme-text-sub` for secondary text,
  `theme-text-muted` for labels; `rise-color` for signed changes (red up, green down).
- Clocks: `gui-seg7` with `theme-seg-on` / `theme-seg-off`.
- Empty states: one muted line at y 60..70, e.g. `(txt 14 70 "暂无数据" theme-text-muted)`.
- Lists: draw only what fits (`(< i 8)`, or stop when `y` passes `(body-bottom)`).

### Fonts

- `F-TEXT` (slot 2) for all text. Chinese text is fine on screen. Avoid `·` and other
  rare symbols: the system fonts may lack them (use spaces).
- `F-BIG` (font 1) only for numbers: `" %-./0123456789:AMP"`.
- More faces: `(use-font id "face" size 'system)` in slots **3..6** (2 is applib's, 7 the
  info pages'). Pattern from the calendar:

  ```scheme
  (define cal-day-font (if (use-font 3 "simhei" 24 'system) 3 F-TEXT))
  ```

  `'system` loads `<face>-<size>` from the board's font ROM (else `/spiffs/fonts/`)
  and returns `#f` when absent. Without `'system` the build embeds a subset that
  holds only ASCII and the characters of the app's **string literals**: text that
  comes from the proxy may be missing glyphs, so embed only for fixed labels.
- The build recognizes only the literal form `(use-font N "face" [size] ['embed|'system])`;
  the new API name `font-use` is never embedded.

### Memory and speed

- The board's VM heap is about 101 KB (firmware 1.0.64). Keep the app's live heap under about 32 KB
  (the test suite checks this with the cached documents). Each cached document is up
  to 4 KB of strings plus a 6-slot vector per item; up to 3 are kept (all pages'
  with a carousel), so keep documents small.
- Bytecode, constants and string literals stay in Flash: big constant tables cost
  no RAM, data built at run time does.
- A render procedure must be **deterministic and cheap**: it can run several times
  per frame (per band when the display list overflows) and for any dirty rows. Do not
  advance state, fetch, or read settings in it; recording layout positions for the
  tick (as the calendar does) is fine.
- Allocation in render is garbage per frame: avoid closures and per-frame lists where
  a loop does; compute lookups and colors once at the top level.
- The display list holds 128 commands statically, grows from the heap to 512; past
  that Scheme runs once per band. The busiest app pages use 70..105 commands. Every
  `txt`, rect, line or circle is one command; prefer fewer, larger shapes.

---

## 6. Writing a proxy endpoint

`tools/app_proxy.py` turns upstream APIs (HTTPS, large JSON) into small documents the
board's JSON feed can hold. Endpoints are plain functions `app_<name>(q)`, where `q`
is the query as `{key: string}`, returning a list of item dicts. `handle()` wraps it
as `{"items": [...]}` at `GET /app/<name>`.

### Item format

- One flat object per item with at most the keys `t`, `a`, `b`, `c`, `d`, `e`; other
  keys are ignored, nested objects and arrays are not captured.
- `t` is required; an object without it is dropped.
- Values: strings. Numbers work (the device sees their text) but `None`, `True` and
  `False` arrive as the text `"null"`, `"true"`, `"false"`: send `''` instead.
- At most 255 bytes per value (`clip()` keeps 250), about 4 KB of values per document
  (each value + 1 byte), at most 64 items. Beyond that the device drops the rest
  silently (`(app-status 'truncated)` is `#t`).
- Series: `series(values, scale=1)` -> `"12,-3,7"` (integers, rounded after scaling).
- Data time: `upd(when=None)` -> `{"t":"upd","a":"HH:MM"}`; pass the upstream's ISO
  time when it has one.
- Failure: `raise ProxyError('message')`. `handle()` turns it (and `ValueError`,
  `KeyError`, `IndexError`) into `[{"t":"error","a":message}]`; the device keeps its
  old data and shows the message in the footer. Other exceptions are not caught.

### Helpers

| Helper | Use |
|---|---|
| `cached(key, ttl, fn)` | `fn()` at most once per `ttl` seconds per key (a tuple of the endpoint and its parameters); on `ProxyError` returns the last good value if any. |
| `http_get(url, headers=None, timeout=10)` | Body bytes, gzip/deflate decoded; `ProxyError` on failure. |
| `get_json(url, headers=None, timeout=10)` | Parsed JSON; `ProxyError` when not JSON. |
| `parallel({name: fn})` | Runs functions concurrently; returns `get(name)`. |
| `clip(text, n=None)` | Folds whitespace; cuts with "…" past `n` characters or the value byte limit. |
| `series(values, scale=1)`, `upd(when=None)`, `hhmm(t=None)` | As above; `hhmm` formats a time as `HH:MM`. |
| `fmt_count(n)`, `fmt_tokens(n)` | `1.2万` / `1.2亿`; `1.2K` / `3.4M` / `1.20B`. |
| `locate(loc)` | `auto` (public IP's city), a city name, or `lon,lat` -> a place (weather, calendar). |

### Example: `/app/hello` for the two-page app

```python
def app_hello(q):
    name = q.get('name', 'World')

    def build():
        today = dt.date.today()
        items = [{'t': 'greet', 'a': clip(f'你好，{name}！'), 'b': str(today.timetuple().tm_yday),
                  'c': clip(name, 8)}]
        for label, change in (('甲', 1.25), ('乙', -0.4), ('丙', 0.0)):
            items.append({'t': 'row', 'a': label, 'b': f'{change:+.2f}'})
        items.append({'t': 'series', 'a': series([3, 5, 2, 8, 6, 9, 7])})
        items.append(upd())
        return items
    return cached(('hello', name), 60, build)
```

Register it in the `APPS` table near the end of the file:

```python
APPS = {'weather': app_weather, 'news': app_news, 'stocks': app_stocks, 'usage': app_usage, 'calendar': app_calendar,
        'hello': app_hello,
        'ping': lambda q: [{'t': 'ok', 'a': hhmm()}]}
```

Names must match `\w+` (the route is `/app/(\w+)`). Check it in a browser:
`http://localhost:8087/app/hello?name=Ada` (the proxy's status page at `/` lists the
requests and the cache). The board sends non-ASCII query text as raw UTF-8 or
percent-encoded (`url-encode`); both decode. Keep the endpoint's cache time and the
page's `interval-ms` in step, and add the endpoint to the module docstring and
`status_page()` list. `tests/test_app_proxy.py` holds the proxy's tests.

---

## 7. Pitfalls

Data

- `items-find`, `items-all` and `items-where` take the type as a **string** (`"day"`);
  `it-t` returns a **symbol** (`'day`). `(items-find items 'day)` is an error.
- Field indexes are 1..5 for `a`..`e` (`it-int`, `it-fixed`, `it-ints`, `it-f`,
  `items-where`); 0 is the type.
- Every field is a string. Compare with `string=?` / `equal?`, never `=`; convert with
  `it-int` / `it-fixed`, which give `#f` for missing or non-numeric text: write
  `(or (it-int it 3) 0)` where a number is required.
- `string->fixed` (behind `it-int` / `it-fixed`) takes `,` as a decimal separator:
  `"1,234"` reads as 1. Send no thousands separators; read lists with `it-ints` or
  `str-part`.
- A missing field is `""` through `it-a`..`it-e` but `#f` in the vector
  (`items-where` compares the raw slot).
- The error item works only as the **first** item. `app-error` is one global: after an
  error, every page's footer shows it until a good document arrives.
- Pages sharing a URL share one feed and the interval of the page that started it.
- `list-min` / `list-max` fail on `'()`: guard as the apps do, `(list-max (cons 0 vals))`.

Time

- `app-sec` includes the colon (`":30"`), is `":--"` before the clock syncs and `"--"`
  before the first tick. `app-date` and `app-week` hold placeholder text before the
  sync, not `""`.
- `app-utc-sec` is the second of the UTC day only once synced; before that it is the
  uptime. Check `(time-synced?)` or `app-time` against `"--:--"`.

Settings and URLs

- URL procedures run once: a changed setting needs an app restart. `(app-start-feed!)`
  refetches but does not rebuild the URL.
- `app-config` reads files on every call; an empty value yields the default.
- `;` and `#` start comments in meta.ini and in the settings ini, also inside values.
- `config-get` falls back to `config.ini` and ignores sections: choose key names that
  do not clash with system keys.
- Feed URLs, keys, rows and values have no fixed size limit (API 1.2); a document
  only has to fit the board's free memory (`json-feed-status` reports `memory` and
  `truncated`). Keep proxy documents to what a page shows.

Code structure

- All files share one global namespace: prefix the app's names, do not redefine
  applib, gui.scm or `info-*` names (weather's unprefixed `deg` or `fr` are only safe
  because nothing else uses them).
- `app-define!` must come after everything its URL procedures need; set
  `app-subtitle-proc`, `app-tick-proc` and `app-render-empty-pages` before it.
- `app-subtitle-proc` runs on page 1 only, with the items; return a string or `#f`.
- A page in `app-render-empty-pages` gets `'()`: every `items-find` returns `#f`.
- `app-long-press` and `app-mid-press` are not called while the hardware pages show (a long
  press closes them, a mid press opens the system settings); `app-touch` is called
  only by the simulator (the firmware's main loop has no touch input path), even
  while the info pages are open.
- `app-tick-proc` keeps running while the info pages show: check `(info-active?)`
  before invalidating.

Drawing

- `F-BIG` draws only `" %-./0123456789:AMP"`; font 0 lacks most Chinese characters;
  the system fonts may lack `·`.
- An embedded font subset has only the characters of the app's string literals.
- Slot 7 belongs to the info pages and is unloaded when they close.
- `line-chart` draws nothing for fewer than 2 values; `bar-chart` clamps values to
  `0..hi`.
- `gui-invalidate` works on whole rows; a render procedure runs for any dirty rows and
  must give the same picture for the same state.

---

## 8. Local pages and the app's own web page

### Local pages

A page that draws the app's own data (no proxy document) is declared with
`(app-local-page name render)`: nothing is fetched, the render procedure always gets
`'()`, `app-subtitle-proc` is called with `'()` on page 1, and the footer shows the
global `app-footer-text` (set it whenever the data changes). Local pages and proxy
pages can be mixed in one app.

`app-data-proc`, when set, is called as `(app-data-proc items)` each time a new
proxy document arrives (after it is cached): use it to keep history the proxy does
not (the notifier stores new messages this way).

### The app's web page

An app may ship `apps/<id>/web.html`. The build gzips it into the image, right
before the meta trailer (`[page.gz][u32 length][u32 FNV-1a]["MSWP"]`, see
`minischeme_device.h`), and adds `apps/lib/appweb.scm` to the app's sources.

- The board serves the running app's page at `GET /app`. Its settings web page shows
  it as the **first tab** (named after the app), the system settings second. The
  page is shown in a frame and sized to its content.
- In the WebREPL, "🌐 应用网页" opens the page of the simulated device's app.
- The marker `<!-- appweb -->` in the page is replaced by `apps/lib/appweb.css` (the
  settings page's look: `section`, `.item`, `.desc`, `.tag`, buttons, `.up` / `.down`)
  and `apps/lib/appweb.js`:

| JavaScript | Use |
|---|---|
| `appCall(name, fields)` | POST `fields` form-encoded to `/api/app/call?fn=name`; resolves to the parsed JSON (or text); throws on `{"success":false,"error":..}` and HTTP errors. |
| `say(text, kind)` | A note at the top; `kind` `ok`, `warn` or `bad`. |
| `attempt(fn)` | Runs `fn`, shows its error with `say`. |
| `$(id)`, `esc(s)` | `getElementById`, HTML escaping. |

The firmware runs each call on the main loop, between frames, as

```scheme
(app-web name body)   ; => the answer string
```

with `name` the call's name and `body` the form text (at most 4 KB; the answer is
cut at 8 KB). A string starting with `{` or `[` is sent as JSON. An error in the
hook answers `{"success":false,"error":...}` with HTTP 500; the screen is redrawn
after every call. From `appweb.scm`:

| Scheme | Result |
|---|---|
| `(web-field body "key")` | The percent-decoded value of a form field, or `#f`. |
| `(web-field-int body "key" default)` | The field as a number, or `default`. |
| `(web-json v)` | JSON text: strings, numbers, `#t`/`#f`, symbols as strings, vectors as arrays, alists with symbol or string keys as objects, other lists as arrays. |
| `(web-ok (cons 'key value) ...)` | `{"success":true,"key":value,...}` |
| `(web-error "message")` | `{"success":false,"error":"message"}` (English messages) |
| `(web-join strings sep)` | The strings joined with `sep`. |

From the memo app (`apps/memo`), which keeps its memos in `/spiffs/apps/memo.txt`:

```scheme
(define (app-web name body)
  (cond
    ((string=? name "list") (memo-answer))
    ((string=? name "add")
     (let ((text (web-field body "text")))
       (if (str-empty? text)
           (web-error "empty memo")
           (begin (memo-add! text (equal? (web-field body "pin") "1")) (memo-answer)))))
    (else (web-error (string-append "unknown call: " name)))))
```

```javascript
const r = await appCall('add', {text: $('text').value, pin: '1'});   // r.memos
```

Notes:

- Calls reach the app only while it runs: in the system settings `/app` answers 404
  and calls fail. With `[system] web_in_app = 0` there is no web page at all while an
  app runs.
- Strings are bytes: the form decoding and JSON keep UTF-8 as it is. Browsers send
  UTF-8; other senders must too.
- Scripts and other tools may call the same endpoint, e.g.
  `curl -d "text=..." "http://<board>/api/app/call?fn=add"` (with
  `-H "X-MiniScheme-Auth: <password>"` on a board with a hardware password).
- The board serves the page as `/app?t=<token>` (firmware 1.0.67+; `/app` redirects there, the token is
  random per boot) and `appCall` / `appFile*` send the token back. A sandboxed page sends no `Referer`,
  so on a board with a hardware password the token is what lets the page's calls in; a page that calls
  `fetch('/api/...')` itself instead of the helpers gets `401`. `tests/test_apps.py` checks this.
- The device's compiler misreads the character literal `#\"`: use `(integer->char 34)`.

---

## 9. App folders and the sandbox

### Where an app lives

| Folder | What |
|---|---|
| `/spiffs/apps/<id>/` | Installed from the app market (and by `tools/install_apps.py`): `app.msbc`, `config.ini`, the app's data |
| `/spiffs/user/<id>/` | Uploaded by the user on the board's web page (system settings mode): not managed by the market; the same id may be installed both ways |
| `/spiffs/app.msbc` | The built-in app (the firmware's default); its data and settings go to `/spiffs/apps/<id>/` |

`config.ini [system] boot_app` names the image (`apps/weather/app.msbc`, `user/x/app.msbc`). `(app-info)` gives
`(path id ini folder)`. Firmware before folders kept `/spiffs/apps/<id>.msbc`, `<id>.ini` and `<id>.*` data
files flat; the first boot of this firmware moves them into the folders (and `boot_app` with them).

### What an app may do

An app is untrusted code, also when it comes from the market. While its own code runs (its top level,
the hooks, `app-web`):

- **Files**: only its folder. A relative path (`"notes.txt"`) is in it; an absolute path must be in it;
  `..` is refused. `app.msbc` / `app.fasl` there are read-only. The folder holds at most
  `config.ini [system] app_storage_kb` (256 KB by default, the image included); `fs-write` past it is an
  error. `fs-list` lists the folder. `load`, `%read-file-string`, `include-bytes`, `dump-fasl`,
  `load-fasl` and `exit` are refused.
- **Settings**: `(config-get key)` reads the app's `config.ini`, then `config.ini` — but never its secrets:
  `wifi_*`, `boot_app` and keys containing `password`, `passwd`, `secret` or `token` answer `#f`.
  `(app-config-set! key value)` writes the app's own settings; `config-set!` is refused.
- **The system**: `wifi-connect`, `wifi-ap`, `wifi-disconnect`, `restart`, `deep-sleep`, `fontrom-add`,
  `fontrom-remove`, `setup-mode-start` and `factory-reset` are refused. Market installs run only in the
  system settings.
- **Network**: `json-feed-start` reaches any `http://` host; `https://` goes through the protocol converter
  (`[app] https_proxy`, else `proxy`: `<it>/fetch?url=...`). The board's own address is refused by its
  API, and the proxy's `/fetch` refuses hosts of the local network.
- **Its web page** (`/app`) is served with `Content-Security-Policy: sandbox`: it runs in an origin of its
  own, so it can call only `/api/app/call` (its app's `app-web`), not the board's API, and cannot read the
  settings page around it.

The system settings and code sent from the REPL (WebREPL, serial) run without these limits: they are
the owner's. A refused call raises an error like
`fs-read: outside the app's folder /spiffs/apps/memo` or
`restart: not allowed for apps (system settings and the REPL only)`.

### The board's API

Every `/api/*` route checks its caller: a browser page of another origin may call it only from the local
network (`localhost`, `127.x`, `10.x`, `172.16-31.x`, `192.168.x`, `*.local`), a sandboxed app page
(`Origin: null`) only `/api/app/call` and `/api/app/file`, and requests from the board itself are
refused. With a hardware password (`[http] http_password`) every route needs it (`X-MiniScheme-Auth`),
except the two app routes called with the page token of the board's own `/app?t=<token>`.

