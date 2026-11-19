# 活动票务与报名系统：实验三代码

这是 C++17 桌面命令行原型。它有真实可运行的交互入口，数据落在本机 SQLite 文件中；无需手机应用或真实支付服务。GUI 不属于实验三强制项，实验二的网页 UI 图在这里转为相同业务的命令行入口。

## 构建和启动

在 PowerShell 中进入本目录，然后运行：

```powershell
.\build.ps1 -RunTests
$env:PATH = 'D:\GCC\mingw64\opt\bin;D:\GCC\mingw64\bin;' + $env:PATH
..\build\ticket_app.exe --db ticket.db
```

使用 MinGW g++、SQLite3、OpenSSL libcrypto；脚本会检查默认安装路径。程序第一次启动会自动创建数据库表。`--db` 推荐传相对路径，尤其当 Windows 终端当前编码不支持中文绝对路径时。若要演示一批固定命令，可追加 `--script 命令文件`。输入 `help` 查看所有命令；含空格的参数用双引号括起。

首次使用时以访客执行 `bootstrap admin 13000000001 Admin12345` 创建唯一初始管理员。随后执行 `login admin admin 13000000001 Admin12345`，再用 `password Admin12345 a1234567` 完成强制改密。改密完成前，除 `password` 外的命令统一提示先改密，`exit` 也不退出；可用 Ctrl+C 中断程序。示例凭据仅供本地演示，正式运行应自行选择并妥善保存。

## 数据库重置
退出程序，确认当时使用的是 --db ticket.db，再到该数据库所在目录运行：
Remove-Item -LiteralPath .\ticket.db,.\ticket.db-wal,.\ticket.db-shm -ErrorAction SilentlyContinue
下次启动会建立空数据库，需要重新执行 bootstrap 创建初始管理员

## 模块与 UML 映射

| 实验二设计 | 代码位置 | 主要职责 |
|---|---|---|
| 用户及角色 | `account.cpp`、`users` 表 | 访客自助注册、四身份三字段登录、PBKDF2 密码摘要、管理员创建工作人员、改密与限制 |
| 活动、类别 | `event.cpp`、`events/categories` 表 | 草稿、审核、发布、取消、查询、一个活动一个场次和票种 |
| 报名、订单、票 | `booking.cpp`、`registrations/orders/tickets` 表 | 原子占位、候补、模拟支付、超时、退票、验票与缺席 |
| 举报、统计、审计 | `governance.cpp`、`reports/audit_logs/notices` 表 | 举报核查、名单分级、30 天计数、CSV 导出、通知 |
| 组件图中的 API 与到期任务 | `main.cpp` 和 `App::maintenance()` | 命令解析、权限入口、启动和相关操作时补偿处理 |
| 关系数据库 | `db.cpp` | SQLite 表结构、外键、唯一索引、事务封装 |

类图中的“匿名用户”在代码里表示没有登录会话，**不是一条数据库用户记录**。注册账号均保存用户名、手机号与密码摘要，不保存邮箱，也不单独存储重复的 `role` 对象；账号类型在表的受约束字段中表示，业务层按角色检查。实验二组件图中的 Web UI/HTTPS API 在此原型中由 CLI 调用服务方法，数据状态及业务边界保持对应。

## 核心规则

- `book` 通过 SQLite `BEGIN IMMEDIATE` 与条件更新占用名额；`occupied` 有数据库范围约束，最后一张票仅能被一个事务取得。
- `request_id` 对报名、候补唯一；重复报名返回同一未结束记录。支付、取消、超时、递补和检票在事务中同步更新相关对象。
- 待支付 15 分钟未确认即超时；释放名额后，放票期内且距活动开始至少 12 小时时递补队首候补者。
- 模拟支付只改变订单和电子票状态；模拟退款不会调用真实支付渠道。
- 票码由随机数与订单标识组成，数据库保证全局唯一，且一个订单最多一张票。
- 结束后 180 天普通业务数据不可查看，底层表和审计日志不物理删除。
- 三项比率按实验一口径计算；购票人数不包括已退款者。分母为零时返回 `-1`。

## 查看类别与活动

输入 `categories` 查看当前可用类别及编号；输入 `events` 查看当前身份可见的活动表格，`events 2 20` 查看第 2 页、每页 20 条。访客仅看到已发布活动；组织者还能看到自己的草稿及审核中活动，管理员可查看全部状态。`search` 提供组合筛选，登录后也可使用。

## 命令示例

```text
register organizer org1 13000000002 Org12345
login organizer org1 13000000002 Org12345
event-new 1 "校园音乐夜" "社团演出" "南京大学" 2026-11-15T19:00 2026-11-15T21:00 2026-11-13T10:00 2026-11-15T18:00 12800 100
event-submit 1
logout
login admin admin 13000000001 NewAdmin12345
review 1 approve -
logout
search category=1 min=0 max=20000 location=南京 "from=2026-11-15T00:00"
register participant alice 13000000003 Alice12345
login participant alice 13000000003 Alice12345
book 1 request-alice-1
pay 1 request-alice-pay-1
mine
```

示例活动时间需按实际运行日期调整；过期日期会被状态检查拒绝。管理员创建类别后才能创建活动。访客与已登录用户都能查看权限范围内的活动。

## 自动测试与代码规模

`tests.cpp` 覆盖注册登录、改密、审核发布、组合查询、报名、候补、支付、退款、超时、签到、举报、统计和 20 人争抢最后一张。测试固定北京时间，以便复现时间边界；运行测试会在构建目录生成 `test_runtime.db`。主程序源码（不计自动生成 GUI 代码及测试）超过 1500 行。统计方式：对 `code/*.cpp`（排除 `tests.cpp`）和 `code/*.hpp` 计物理行。

## 目前的实现边界

这是课程原型。到期处理在启动、相关操作或手动执行 `maintenance` 时补偿运行，没有常驻后台服务。CLI 会显示输入的密码，因此真实部署应换成遮挡输入的桌面界面或安全终端。并发依赖 SQLite 写事务，适用于单机原型。网络故障界面在实验二 UI 中展示；本地 CLI 没有网络传输层，持久化失败会返回错误，不显示虚假的成功。源代码中没有移动端、真实支付、短信、第三方账号或邮件服务。
