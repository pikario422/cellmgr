# CellMgr

CellMgr 是一个极轻量的模组管理后台，后端用 C 语言实现，前端使用原生 HTML/CSS/JavaScript，不依赖前端框架。

当前默认 profile 面向 Fibocom FM650，但整体设计支持通过 JSON profile 接入更多模组。

## 功能

- 模组概览：CPU、内存、磁盘、温度、运行时间、WAN 流量
- ofono D-Bus 封装：`dbus-send`、`dbus-monitor`
- AT 指令台：通过 `org.ofono.Modem.SendAtcmd`
- profile 前端增删改查、导入、导出、激活
- FM650 高级网络：
  - `AT+GTACT=?` 频段探测
  - `AT+GTACT=...` 锁频段
  - `AT+GTCELLLOCK=1,<rat>,0,<earfcn>,<pci>` 锁小区
  - `AT+GTCELLLOCK=1,<rat>,1,<earfcn>` 锁频点
  - `AT+GTCELLLOCK=0` 解除锁定
- 短信：查看、读取、发送、删除
- 短信转发：SMTP relay 或 HTTP webhook
- D-Bus 事件监听缓存
- 立即重启、定时重启
- 可选登录认证

## 目录

```text
src/                  C 后端源码
config/cellmgrd.conf  默认配置
profiles/fm650.json   默认 FM650 profile
web/mock.html         静态 mock 预览页
.github/workflows/    GitHub 自动构建
```

## 依赖版本

GitHub Actions 使用固定版本 SQLite amalgamation 构建，不依赖 runner 自带 SQLite。

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| SQLite | 3.46.1 | CI 下载官方 `sqlite-amalgamation-3460100` 并静态编译 |
| C 标准 | C99 | 使用 POSIX API |
| pthread | 目标工具链自带 | 用于后台线程 |
| ofono | 运行时环境自带 | 需要暴露 modem path 和 `SendAtcmd` |
| dbus-send / dbus-monitor | 运行时环境自带 | 仅作封装调用 |
| curl | 运行时环境自带 | 仅用于 HTTP webhook，不用于 SMTP |

## 构建

### 可复现构建

```sh
export SQLITE_VERSION=3460100
export SQLITE_YEAR=2024
mkdir -p third_party/sqlite .deps
curl -fL "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip" -o .deps/sqlite.zip
unzip -q .deps/sqlite.zip -d .deps
cp ".deps/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.c" third_party/sqlite/
cp ".deps/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.h" third_party/sqlite/
make USE_BUNDLED_SQLITE=1 SQLITE_DIR=third_party/sqlite
```

### 使用系统 SQLite

```sh
make
```

### 目标板交叉编译

```sh
make CC=aarch64-unisoc-linux-gnu-gcc USE_BUNDLED_SQLITE=1 SQLITE_DIR=third_party/sqlite
```

## 运行

```sh
./cellmgrd -c config/cellmgrd.conf
```

默认监听：

```text
http://<设备地址>:4242/
```

## Mock 预览

直接打开：

```text
web/mock.html
```

这是静态 mock 页面，只用于看布局和交互，不会调用后端。

## 说明

- 默认不使用 `system()` 和 `popen()`
- AT 指令有危险指令过滤
- `FM650_AT_Commands_full.txt` 不提交到仓库，避免版权问题
- TLS SMTP 不做直连，建议使用本地 SMTP relay 或 HTTP webhook
