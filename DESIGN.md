# FM650 模组管理后台设计

## 目标

做一个极致轻量化的本机 Web 管理后台：

- 后台使用 C 语言实现，单进程可部署，优先复用系统已有能力，避免重复造轮子。
- 前端使用原生 HTML/CSS/JavaScript，不依赖 npm、Vue、React、Bootstrap 等资源。
- 设备信息直接从 `/proc`、`/sys`、系统文件和必要命令读取。
- 模组管理优先通过 ofono 的 D-Bus 接口完成；AT 指令优先通过 `org.ofono.Modem.SendAtcmd` 执行。
- 页面包含概览、设备监控、模组监控、高级网络、短信管理、AT 指令台、系统设置。

## 目标设备约束

目标设备已知基础库较少：

- glibc 2.27、pthread、rt、dl、m、zlib 等基础库可用。
- 未确认存在 `libdbus`、`libglib`、`libmicrohttpd`、`openssl` 开发库。
- SQLite 可以使用，用于配置、短信、AT 历史、定时任务、设备 profile。
- curl 7.61.0 不支持 SMTP 协议，只支持 HTTP/HTTPS/FTP 等协议。

设计结论：

- HTTP 服务自己用 POSIX socket 实现。
- JSON 生成和请求解析自己实现小型工具函数。
- 使用 SQLite 做轻量持久化；配置导入导出使用 JSON。
- 不依赖 curl 发送 SMTP；短信邮件转发使用内置极简 SMTP 客户端，或者调用用户配置的 HTTP webhook。
- ofono 接入优先复用系统命令：`dbus-send` 做同步调用，`dbus-monitor` 做信号监听。
- 后续只有在系统缺少 D-Bus 命令或性能不够时，才考虑内置最小 D-Bus 客户端。

## 进程模型

后台进程名建议为 `cellmgrd`。

含义是 cellular manager daemon，比 `fm650d` 更通用，便于以后接入 FG650、移远、广和通其他系列或非 FM650 设备。

默认监听：

- `0.0.0.0:4242`，开发和内网调试使用。
- 量产建议改为 `127.0.0.1:4242` 加 nginx/uhttpd 反代，或开启后台自带账号密码。

线程：

- 主线程：HTTP accept、轻量请求分发。
- 采样线程：每 1 秒采集 CPU、内存、磁盘、温度、上下行速率。
- ofono 调用线程：封装 `dbus-send`，维护 ofono/modem 状态缓存，串行执行 ofono 操作。
- ofono 信号线程：封装 `dbus-monitor`，监听 modem、网络、短信信号。
- AT 线程：默认通过 ofono `SendAtcmd` 串行执行；直连串口只作为 fallback。

原则：

- HTTP 请求不直接阻塞在慢操作上。
- 状态类接口读缓存。
- 写操作进入任务队列，返回执行结果或任务 ID。
- AT、锁频、锁小区、重启等危险操作必须串行。

## 文件布局

```text
FM650-tools/
  DESIGN.md
  Makefile
  src/
    main.c
    http.c
    http.h
    json.c
    json.h
    config.c
    config.h
    auth.c
    auth.h
    sysinfo.c
    sysinfo.h
    netdev.c
    netdev.h
    ofono.c
    ofono.h
    dbus_cmd.c
    dbus_cmd.h
    dbus_monitor.c
    dbus_monitor.h
    at.c
    at.h
    sms.c
    sms.h
    smtp.c
    smtp.h
    db.c
    db.h
    profile.c
    profile.h
    tasks.c
    tasks.h
    web_assets.c
    web_assets.h
  web/
    index.html
    app.js
    style.css
  config/
    cellmgrd.conf
  profiles/
    fm650.json
```

为了部署轻，最终可以把 `web/` 编译进 `web_assets.c`，设备上只放一个 `cellmgrd`、一个 SQLite 数据库和可选 profile JSON。

## 持久化

使用 SQLite，默认路径：

- `/etc/cellmgr/cellmgr.db`
- 开发模式可用 `./data/cellmgr.db`

用途：

- `settings`：系统设置、监听地址、认证、转发配置。
- `device_profiles`：设备能力 profile，JSON 文本保存，支持导入导出。
- `capability_cache`：AT 自动探测结果，例如可用频段、锁小区参数范围、短信能力。
- `sms_messages`：短信缓存、转发状态、删除标记。
- `at_history`：AT 指令历史和响应。
- `tasks`：定时重启、异步操作记录。
- `samples`：可选短期监控采样，默认只保留最近 1 小时。

SQLite 编译：

- 如果交叉工具链有 `sqlite3.h` 和 `libsqlite3`，直接链接 `-lsqlite3`。
- 如果目标机只有运行库而无开发头文件，可以把官方 `sqlite3.c/sqlite3.h` 作为 amalgamation 放入 `third_party/sqlite/` 编译。
- 如果用户明确要求极限体积，可以编译时关闭 `samples` 持久化，只保留配置和历史。

`capability_cache` 建议字段：

```sql
CREATE TABLE capability_cache (
  profile_id TEXT NOT NULL,
  capability TEXT NOT NULL,
  modem_path TEXT NOT NULL,
  raw_response TEXT NOT NULL,
  parsed_json TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  ttl_sec INTEGER NOT NULL,
  PRIMARY KEY (profile_id, capability, modem_path)
);
```

## 设备 Profile

后台不把 FM650 AT 指令写死在业务代码里，而是使用 profile 描述“能力”、“命令模板”、“探测方式”和“解析器”。

重要边界：

- profile 保存模块差异：用什么 AT 指令、参数模板、返回格式、解析器名称、危险等级。
- SQLite 缓存运行时数据：当前支持的频段、当前锁定状态、当前小区、最近一次短信、AT 历史。
- 前端展示的数据优先从设备实时探测或缓存读取，不把某台设备的实际能力静态写死在 profile。
- 类似 `band_maps` 这种“支持哪些频段”的静态列表不作为主配置；但 `display_rules` 可以留在 profile 中，用于把 AT 返回的数字编码显示成人类可读名称。

默认 profile：

- `profiles/fm650.json`
- 启动时导入到 SQLite。
- 系统设置页面支持查看、编辑、导入、导出、切换当前 profile。

配置建议：

- SQLite 保存当前启用 profile 和运行配置。
- JSON 作为人可读、人可迁移的设备能力描述。
- 系统设置里提供 JSON 编辑器，但保存前必须做字段校验和一次 `test` 探测。

profile 示例：

```json
{
  "id": "fibocom-fm650",
  "name": "Fibocom FM650",
  "modem_path": "/ril_0",
  "at_backend": "ofono-sendatcmd",
  "dbus": {
    "destination": "org.ofono",
    "send_at_interface": "org.ofono.Modem",
    "send_at_method": "SendAtcmd"
  },
  "capabilities": {
    "imei": {
      "command": "AT+CGSN",
      "parser": "line_after_echo"
    },
    "signal_basic": {
      "command": "AT+CSQ",
      "parser": "csq"
    },
    "operator": {
      "command": "AT+COPS?",
      "parser": "cops"
    },
    "eps_registration": {
      "command": "AT+CEREG?",
      "parser": "cereg"
    },
    "nr_registration": {
      "command": "AT+C5GREG?",
      "parser": "c5greg"
    },
    "band_select": {
      "read": "AT+GTACT?",
      "test": "AT+GTACT=?",
      "set_template": "AT+GTACT={mode},{rat},{pref},{bands}",
      "discovery": {
        "command": "AT+GTACT=?",
        "cache_ttl_sec": 86400
      },
      "display_rules": {
        "lte_band_code": "fibocom_lte_offset_100",
        "nr_band_code": "fibocom_nr_prefix_50",
        "examples": {
          "101": "B1",
          "141": "B41",
          "5010": "n10"
        }
      },
      "parser": "gtact"
    },
    "cell_lock": {
      "read": "AT+GTCELLLOCK?",
      "test": "AT+GTCELLLOCK=?",
      "set_template": "AT+GTCELLLOCK={mode},{rat},{type},{earfcn},{pci}",
      "frequency_template": "AT+GTCELLLOCK={mode},{rat},1,{earfcn}",
      "unlock": "AT+GTCELLLOCK=0",
      "discovery": {
        "command": "AT+GTCELLLOCK=?",
        "cache_ttl_sec": 86400
      },
      "parser": "gtcelllock"
    },
    "sms_list": {
      "command": "AT+CMGL={status}",
      "parser": "cmgl"
    },
    "sms_read": {
      "command": "AT+CMGR={index}",
      "parser": "cmgr"
    },
    "sms_delete": {
      "command": "AT+CMGD={index}",
      "parser": "ok_error"
    },
    "sms_send": {
      "command": "AT+CMGS={number}",
      "parser": "cmgs"
    },
    "qci": {
      "command": "AT+CGEQOS?",
      "parser": "cgeqos"
    }
  }
}
```

说明：

- `parser` 是后台内置的小解析器名称，不在 JSON 里执行脚本，避免安全风险。
- `set_template` 只允许替换白名单变量，变量先做类型和范围校验。
- `discovery.command` 用于自动获取支持范围，例如 FM650 的 `AT+GTACT=?` 可以发现支持的 RAT 和频段，`AT+GTCELLLOCK=?` 可以发现锁小区参数范围。
- `display_rules` 只是显示兜底，不表示设备支持这些频段；实际可用列表仍以 `discovery.command` 返回为准。
- 自动发现结果写入 SQLite 的 `capability_cache`，带 TTL；前端可手动点击“重新探测”。
- FM650 当前 profile 依据 `FM650_AT_Commands_full.txt` 初始化，重点覆盖 `+GTACT`、`+GTCELLLOCK`、`+CSQ`、`+CREG`、`+CGREG`、`+CEREG`、`+C5GREG`、`+COPS`、`+CGEQOS`、`+CNMI`、`+CMGL`、`+CMGR`、`+CMGD`、`+CMGS`、`+CGDCONT`、`+CGACT`、`+CGSN`。

### 频段发现

可用频段不写死，流程如下：

1. 后台读取当前 profile 的 `network.band.discovery.command`。
2. 通过 ofono `SendAtcmd` 执行，例如 FM650：`AT+GTACT=?`。
3. 使用 profile 指定的 `gtact` 解析器提取支持的 RAT、preferred RAT、LTE band、NR band 等。
4. 解析结果保存到 SQLite `capability_cache`，页面读取缓存并显示“最后探测时间”。
5. 用户锁频时，前端只能从探测结果中选择；如果用户手动输入，后台仍按 parser/range 校验。

FM650 的 band 编码可以通过 `+GTACT=?` 自动获取；显示名称优先使用 AT 返回内容。如果返回只有数字编码，则由 `gtact` 解析器按 FM650 规则生成显示名，例如 LTE `101` 显示为 `B1`，LTE `141` 显示为 `B41`，NR `501` 显示为 `n1`，NR `5010` 显示为 `n10`。

## 数据来源

### 设备监控

- CPU 使用率：读取 `/proc/stat` 两次采样差值。
- 内存：读取 `/proc/meminfo`。
- 磁盘：`statvfs("/")` 和可配置挂载点。
- 负载：读取 `/proc/loadavg`。
- 运行时间：读取 `/proc/uptime`。
- 温度：扫描 `/sys/class/thermal/thermal_zone*/temp`，同时读取 `type`。
- 网络速率：读取 `/proc/net/dev`，按接口累计 RX/TX 字节差值。
- 清理内存：执行 `sync` 后写 `/proc/sys/vm/drop_caches`，需要 root。

### 模组监控

ofono 标准能力：

- Modem：电源、在线状态、型号、厂家、序列号、接口路径。
- SIM：IMSI、ICCID、PIN 状态、运营商相关信息。
- NetworkRegistration：注册状态、运营商、制式、信号强度、小区信息。
- ConnectionManager：数据连接、APN、接入技术。
- MessageManager：短信收发。

厂商或制式增强能力：

- QCI、上下行物理层速率、频段、EARFCN/NRARFCN、PCI、TAC、RSRP、RSRQ、SINR。
- 锁频段、锁小区、锁频点。

这些能力通常 ofono 标准接口不完整，设计为 AT 扩展：

- `at_query_radio_metrics()`
- `at_query_band_list()`
- `at_lock_band()`
- `at_lock_cell()`
- `at_lock_frequency()`
- `at_unlock_network()`

具体 AT 指令需要按 FM650 固件实际支持命令补齐。

## ofono 接入策略

核心原则：先封装，少造轮子。

当前设备已经验证可用的命令会作为第一版 ofono 后端：

```sh
dbus-send --system --print-reply \
  --dest=org.ofono /ril_0 org.ofono.Modem.GetProperties

dbus-send --system --print-reply \
  --dest=org.ofono /ril_0 org.ofono.NetworkRegistration.GetProperties

dbus-send --system --print-reply \
  --dest=org.ofono /ril_0 org.ofono.SimManager.GetProperties

dbus-send --system --print-reply \
  --dest=org.ofono /ril_0 org.ofono.Modem.SetProperty \
  string:"Online" variant:boolean:false

dbus-send --system --print-reply \
  --dest=org.ofono /ril_0 org.ofono.Modem.SendAtcmd \
  string:"AT+CGSN"
```

信号监听：

```sh
dbus-monitor --system "sender='org.ofono'"
dbus-monitor --system "destination='org.ofono'"
dbus-monitor --system "interface='org.ofono.MessageManager'"
```

后台封装：

- `dbus_cmd_call()`：构造并执行 `dbus-send`，捕获 stdout/stderr/退出码。
- `ofono_get_modem_properties()`：封装 `org.ofono.Modem.GetProperties`。
- `ofono_get_network_properties()`：封装 `org.ofono.NetworkRegistration.GetProperties`。
- `ofono_get_sim_properties()`：封装 `org.ofono.SimManager.GetProperties`。
- `ofono_set_modem_online()`：封装 `org.ofono.Modem.SetProperty Online`。
- `ofono_send_at()`：封装 `org.ofono.Modem.SendAtcmd`。
- `dbus_monitor_start()`：启动 `dbus-monitor` 子进程并解析 ofono 信号。

安全注意：

- 调用 `dbus-send` 不通过 shell 拼接整条命令，使用 `fork + execvp` 参数数组，避免命令注入。
- AT 指令作为单独参数传给 `dbus-send` 的 `string:...` 参数，内部仍要做危险指令过滤。
- `dbus-monitor` 是长进程，异常退出后退避重启。

后续可选：

- 如果某些设备没有 `dbus-send`/`dbus-monitor`，再实现 `dbus_min.c`。
- `dbus_min.c` 只作为兼容后端，不作为第一版默认路径。

## AT 指令后端

配置项：

```ini
at_backend=ofono-sendatcmd
ofono_modem_path=/ril_0
at_device=/dev/ttyUSB2
at_baud=115200
at_timeout_ms=3000
```

行为：

- 所有 AT 指令通过一个独占 worker 串行执行。
- 默认后端是 ofono `org.ofono.Modem.SendAtcmd`。
- 直连串口后端只用于 ofono 不支持 `SendAtcmd` 的设备。
- 用户 AT 指令台默认禁止危险指令，可在配置开启高级模式。
- 后台内部 AT 操作带来源标记，日志记录调用接口和时间。
- 响应最多保留 8 KB，避免页面或内存被超长响应拖垮。

危险指令策略：

- 默认拦截 `AT+CFUN=1,1`、擦写 NV、恢复出厂、改 IMEI、关机等模式。
- 高级模式开启后仍需二次确认。

## HTTP API

统一返回：

```json
{
  "ok": true,
  "data": {},
  "error": null
}
```

错误返回：

```json
{
  "ok": false,
  "data": null,
  "error": {
    "code": "BAD_REQUEST",
    "message": "invalid parameter"
  }
}
```

### 页面资源

- `GET /`：管理后台首页。
- `GET /app.js`：前端逻辑。
- `GET /style.css`：样式。
- `POST /api/auth/login`：认证开启时登录并返回 session token。

### 概览

- `GET /api/overview`

返回设备、模组、SIM、网络、流量、温度、运行时间等一屏信息。

### 设备监控

- `GET /api/device/status`
- `POST /api/device/drop-caches`

### 模组监控

- `GET /api/modem/status`
- `GET /api/modem/radio`
- `POST /api/modem/power`
- `POST /api/modem/online`

### 高级网络

- `GET /api/network/bands`
- `POST /api/network/lock-band`
- `POST /api/network/lock-cell`
- `POST /api/network/lock-frequency`
- `POST /api/network/unlock`

请求示例：

```json
{
  "rat": "lte",
  "bands": [1, 3, 5, 8, 40, 41]
}
```

```json
{
  "rat": "lte",
  "earfcn": 1850,
  "pci": 123
}
```

### 短信管理

- `GET /api/sms/list`
- `GET /api/sms/read?index=<index>`
- `POST /api/sms/send`
- `POST /api/sms/delete`
- `POST /api/sms/forward-test`

短信优先级：

- 优先使用 ofono `MessageManager` 管理短信和监听短信信号。
- 如果某个设备 ofono 短信接口不完整，再按 profile 里的 `CMGL/CMGR/CMGD/CMGS/CNMI` AT 能力 fallback。

短信转发配置：

```ini
sms_forward_enabled=0
sms_forward_mode=smtp
smtp_host=smtp.example.com
smtp_port=587
smtp_starttls=0
smtp_user=
smtp_pass=
smtp_from=fm650@example.com
smtp_to=admin@example.com
```

说明：

- curl 不支持 SMTP，所以不使用 curl 发邮件。
- 极简 SMTP 客户端第一版支持明文 SMTP 和 AUTH LOGIN。
- 如必须支持 TLS SMTP，目标机 OpenSSL 只有运行库但未确认开发头文件，建议通过局域网邮件网关或 HTTPS webhook 转发。

### AT 指令台

- `POST /api/at/send`
- `GET /api/at/history`
- `GET /api/at/capabilities`
- `POST /api/at/test-capability`

请求：

```json
{
  "command": "AT+CSQ",
  "timeout_ms": 3000
}
```

测试 profile 能力：

```json
{
  "capability": "cell_lock",
  "mode": "read"
}
```

### 系统设置

- `GET /api/system/settings`
- `POST /api/system/settings-save`
- `POST /api/system/reboot`
- `POST /api/system/scheduled-reboot`
- `POST /api/system/password`
- `GET /api/system/dbus/commands`
- `GET /api/system/dbus/events`
- `POST /api/system/dbus/test`
- `GET /api/profiles`
- `GET /api/profiles/get?id=<profile_id>`
- `GET /api/profiles/current`
- `POST /api/profiles/save`
- `POST /api/profiles/import`
- `POST /api/profiles/export`
- `POST /api/profiles/activate`
- `POST /api/profiles/delete`
- `POST /api/profiles/validate`

D-Bus 诊断页内置常用命令：

- 查看 Modem 属性：`org.ofono.Modem.GetProperties`
- 查看网络状态：`org.ofono.NetworkRegistration.GetProperties`
- 查看 SIM 卡信息：`org.ofono.SimManager.GetProperties`
- 设置飞行模式/在线状态：`org.ofono.Modem.SetProperty Online`
- 发送 AT 指令：`org.ofono.Modem.SendAtcmd`
- 监听 ofono 所有信号：`dbus-monitor --system "sender='org.ofono'"`
- 监听发给 ofono 的调用：`dbus-monitor --system "destination='org.ofono'"`
- 监听短信信号：`dbus-monitor --system "interface='org.ofono.MessageManager'"`

后台会启动一个 `dbus-monitor --system "sender='org.ofono'"` 监听线程，把 ofono 信号行写入 SQLite `dbus_events`，供 `GET /api/system/dbus/events` 查看。

定时重启不依赖 cron：

- 后台保存计划。
- 采样线程每分钟检查当前时间。
- 到点后执行 `sync` 和 `reboot`。

## 前端设计

整体风格：

- 单页应用，左侧导航，右侧内容。
- 默认进入“概览”。
- 配色克制，信息密度高，不做营销式大卡片。
- 移动端使用顶部标签/抽屉导航。
- 所有危险动作使用确认弹窗。

页面：

- 模组概览：设备名、运行时间、CPU/内存/磁盘、温度、运营商、注册状态、信号、制式、IP、SIM、上下行速率。
- 设备监控：CPU 曲线、内存、磁盘、温度、网络接口速率、清理内存按钮。
- 模组监控：运营商、SIM/ICCID/IMSI、QCI、RAT、RSRP/RSRQ/SINR、EARFCN/NRARFCN、PCI、TAC、上下行速率。
- 高级网络：可用频段列表、锁频段、锁小区、锁频点、解除锁定。
- 短信管理：收件箱、短信详情、发送短信、邮件转发配置和测试。
- AT 指令台：输入框、发送按钮、响应窗口、历史记录。
- 系统设置：登录密码、监听地址、当前设备 profile、JSON profile 编辑/导入/导出、D-Bus 诊断、定时重启、立即重启。

刷新策略：

- 概览：2 秒刷新。
- 设备监控：1 秒刷新。
- 模组监控：2 秒刷新。
- 短信：手动刷新，可选 10 秒轮询。
- AT 和设置：手动操作。

## 配置文件

路径建议：

- `/etc/cellmgr/cellmgrd.conf`
- 开发模式可用 `./config/cellmgrd.conf`

示例：

```ini
listen_host=0.0.0.0
listen_port=4242
auth_enabled=1
auth_user=admin
auth_pass_hash=
ofono_mode=dbus
ofono_backend=dbus-send
ofono_modem_path=/ril_0
dbus_socket=/var/run/dbus/system_bus_socket
at_backend=ofono-sendatcmd
at_device=/dev/ttyUSB2
at_baud=115200
wan_iface=rmnet_data0
sample_interval_ms=1000
allow_dangerous_at=0
```

密码：

- 不保存明文密码。
- 第一版可使用 SHA-256 哈希；如果不引入加密库，则实现一个小型 SHA-256。
- 登录后使用随机 session token，保存在内存，Cookie 使用 `HttpOnly; SameSite=Strict`。

## 安全边界

- 默认开启登录。
- API 只接受同源请求。
- 修改类接口必须校验 session。
- AT 指令台默认只允许 `AT`、`ATI`、`AT+CSQ`、`AT+COPS?`、`AT+CREG?`、`AT+CEREG?`、`AT+CGDCONT?` 等查询类指令。
- 锁频、重启、清内存、发送短信需要确认字段：

```json
{
  "confirm": true
}
```

## 编译部署

目标交叉编译示例：

```sh
aarch64-unisoc-linux-gnu-gcc -Os -s -pthread -o cellmgrd src/*.c -lsqlite3
```

优化：

- `-Os` 减小体积。
- `-ffunction-sections -fdata-sections -Wl,--gc-sections` 去掉未用函数。
- 不使用 C++ 和大型库。

systemd 或 init 脚本：

```sh
/usr/bin/cellmgrd -c /etc/cellmgr/cellmgrd.conf
```

## 实现顺序

1. 建 SQLite schema、配置加载、FM650 默认 profile 导入。
2. 实现最小 HTTP 服务和内嵌前端，默认监听 `4242`。
3. 实现 `dbus-send` 封装和 ofono 基础接口：Modem、NetworkRegistration、SimManager、SetProperty。
4. 实现 ofono `SendAtcmd` 后端和 AT 指令台。
5. 实现 `/api/device/status`、`/api/overview`，先显示设备侧和 ofono 基础数据。
6. 基于 FM650 profile 实现高级网络：`+GTACT`、`+GTCELLLOCK`。
7. 实现短信：优先 ofono MessageManager，AT 短信作为 fallback，补邮件/HTTP 转发。
8. 实现 `dbus-monitor` 信号监听：ofono、发给 ofono 的调用、短信信号。
9. 加登录、权限、危险操作确认、profile JSON 导入导出。
10. 可选实现内置最小 D-Bus 客户端，作为无 `dbus-send/dbus-monitor` 设备的兼容后端。

## 待确认项

- 目标系统是否带 `sqlite3` 运行库；如果没有，使用 sqlite amalgamation 静态编译。
- 目标系统是否稳定提供 `dbus-send` 和 `dbus-monitor`。
- FM650 ofono modem path 是否固定为 `/ril_0`，还是需要启动时自动发现。
- FM650 `+GTACT`、`+GTCELLLOCK` 在当前固件上的实测参数范围。
- 数据网卡名，例如 `rmnet_data0`、`wwan0`、`usb0`。
- 温度节点路径和含义。
- 邮件转发是否必须直连 TLS SMTP，还是可以使用局域网 SMTP relay/HTTP webhook。
