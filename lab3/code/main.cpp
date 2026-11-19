#include "app.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ticket {
namespace {

template <typename T>
T read(std::istringstream& input, const std::string& label) {
    T result{};
    if (!(input >> result)) {
        throw BusinessError("缺少或错误的参数：" + label);
    }
    return result;
}

std::string word(std::istringstream& input, const std::string& label) {
    std::string result;
    if (!(input >> std::quoted(result))) {
        throw BusinessError("缺少参数：" + label);
    }
    return result;
}

Id require_login(Id session) {
    if (session == 0) {
        throw BusinessError("请先选择身份并登录");
    }
    return session;
}

void print_event(const Event& e) {
    std::cout << "活动 #" << e.id << "  " << e.name << "  [" << e.status << "]\n"
              << "  场次: " << format_beijing_time(e.start_at) << " ~ "
              << format_beijing_time(e.end_at) << "\n"
              << "  放票: " << format_beijing_time(e.sale_start) << " ~ "
              << format_beijing_time(e.sale_end) << "\n"
              << "  票价: " << format_money(e.price_cents) << " 元  "
              << "剩余: " << (e.capacity - e.occupied) << "/" << e.capacity
              << "  地点: " << e.location << "\n";
}

void print_booking(const Booking& b) {
    std::cout << "报名 #" << b.registration_id << "  活动 #" << b.event_id
              << "  状态: " << b.registration_status << "\n";
    if (b.order_id != 0) {
        std::cout << "  订单 #" << b.order_id << "  " << b.order_status
                  << "  支付截止: " << format_beijing_time(b.pay_deadline) << "\n";
    }
    if (!b.ticket_code.empty()) {
        std::cout << "  电子票码: " << b.ticket_code << "\n";
    }
}

void print_stats(const Stats& s) {
    std::cout << "活动 #" << s.event_id << "  总容量 " << s.capacity
              << "  剩余 " << s.remaining << "\n"
              << "待支付 " << s.pending_payment << "  售出票 " << s.sold
              << "  候补 " << s.waitlisted << "  签到 " << s.checked_in
              << "  缺席 " << s.absent << "\n"
              << std::fixed << std::setprecision(3)
              << "签到率 " << s.attendance_rate << "  缺席率 "
              << s.absence_rate << "  容量利用率 " << s.utilization_rate << "\n";
}

void help() {
    std::cout << R"HELP(
活动票务与报名系统（桌面命令行原型）
含空格的文本请用双引号括起。时间为北京时间 YYYY-MM-DDTHH:MM。

访客：
  bootstrap 用户名 手机号 密码          仅空数据库可创建初始管理员
  register participant|organizer 用户名 手机号 密码
  login 身份 用户名 手机号 密码       身份须与账号一致，三个字段均必填
  categories                          列出当前可用活动类别及ID
  events [页码] [每页条数]             列出可见活动，默认每页20条
  search [key=value ...]                可选 category,min,max,from,to,location,
                                        status,order,page,size；条件按 AND 组合

登录后：
  logout                              回到访客
  password 旧密码 新密码
  notices                             查看站内提示
  mine                                查看本人的报名、订单及票
  list 活动ID                         按角色查看参与者名单

组织者：
  event-new 类别ID 名称 简介 地点 开始 结束 放票开始 放票结束 票价分 容量
  event-edit 活动ID 类别ID 名称 简介 地点 开始 结束 放票开始 放票结束 票价分 容量
  event-desc 活动ID 新简介
  event-submit 活动ID
  event-withdraw 活动ID
  event-cancel 活动ID

参与者：
  book 活动ID 请求ID                  有票时创建待支付订单
  waitlist 活动ID 请求ID              售罄时自愿加入候补
  pay 报名ID 请求ID                   点击确认模拟支付
  cancel 报名ID 请求ID                取消或模拟退款
  report 活动ID 原因                  提交活动举报

检票人员：
  checkin 活动ID 票码 请求ID           有效时段内验票签到

管理员：
  staff checker|admin 用户名 手机号 临时密码
  reset 用户ID 临时密码
  restrict 用户ID 0|1 原因
  category-add 名称
  category-disable 类别ID
  review 活动ID approve|reject 原因
  decide 举报ID true|false 原因
  audit 活动ID或0
  risk 用户ID                         查看最近30天缺席、主动退票或违规计数

统计与导出：
  stats 活动ID
  export 活动ID statistics|list|audit 文件路径
  maintenance                         处理到期订单和缺席
  time                                显示服务器时钟
  help / exit
)HELP";
}

EventInput input_event(std::istringstream& input) {
    EventInput result;
    result.category_id = read<Id>(input, "类别ID");
    result.name = word(input, "活动名称");
    result.description = word(input, "活动简介");
    result.location = word(input, "地点");
    result.start_at = parse_beijing_time(word(input, "开始时间"));
    result.end_at = parse_beijing_time(word(input, "结束时间"));
    result.sale_start = parse_beijing_time(word(input, "放票开始"));
    result.sale_end = parse_beijing_time(word(input, "放票结束"));
    result.price_cents = read<int>(input, "票价分");
    result.capacity = read<int>(input, "容量");
    return result;
}

bool execute_line(App& app, Id& session, const std::string& line) {
    if (line.empty() || line.front() == '#') {
        return true;
    }
    std::istringstream input(line);
    const std::string command = word(input, "命令");
    if (session != 0 && app.user(session).must_change_password &&
        command != "password") {
        std::cout << "请先改密：输入 password 旧密码 新密码。\n";
        return true;
    }
    if (command == "exit" || command == "quit") return false;
    if (command == "help") {
        help();
    } else if (command == "time") {
        std::cout << "北京时间 " << format_beijing_time(app.now()) << '\n';
    } else if (command == "bootstrap") {
        if (session) throw BusinessError("请先登出");
        const auto username = word(input, "用户名");
        const auto phone = word(input, "手机号");
        const auto password = word(input, "密码");
        const Id id = app.bootstrap_admin(username, phone, password);
        std::cout << "初始管理员 #" << id << '\n';
    } else if (command == "register") {
        if (session) throw BusinessError("只有访客可以注册");
        const auto role = word(input, "注册身份");
        const auto username = word(input, "用户名");
        const auto phone = word(input, "手机号");
        const auto password = word(input, "密码");
        const Id id = app.register_user(role, username, phone, password);
        std::cout << "注册成功，用户 #" << id << '\n';
    } else if (command == "login") {
        if (session) throw BusinessError("请先登出当前账号");
        const auto role = word(input, "登录身份");
        const auto username = word(input, "用户名");
        const auto phone = word(input, "手机号");
        const auto password = word(input, "密码");
        const User actor = app.login(role, username, phone, password);
        session = actor.id;
        std::cout << "登录成功：" << actor.username << " [" << actor.role << "]\n";
        if (actor.must_change_password) {
            std::cout << "首次登录须先执行 password 命令修改临时密码。\n";
        }
    } else if (command == "logout") {
        require_login(session);
        session = 0;
        std::cout << "已登出，当前为访客。\n";
    } else if (command == "password") {
        const auto old_password = word(input, "旧密码");
        const auto new_password = word(input, "新密码");
        app.change_password(require_login(session), old_password, new_password);
        std::cout << "密码修改成功。\n";
    } else if (command == "categories") {
        const auto rows = app.categories();
        for (const auto& row : rows) {
            std::cout << row.id << " | " << row.name << '\n';
        }
        std::cout << "当前可用类别 " << rows.size() << " 种。\n";
    } else if (command == "events") {
        SearchFilter filter;
        std::string value;
        if (input >> value) {
            filter.page = std::stoi(value);
            if (input >> value) {
                filter.page_size = std::stoi(value);
                if (input >> value) throw BusinessError("events 最多接受页码和每页条数");
            }
        }
        const auto rows = app.search_events(
            filter, session ? std::optional<Id>(session) : std::nullopt);
        std::cout << "ID | 类别ID | 名称 | 场次开始 | 票价(元) | 剩余/容量 | 状态\n";
        for (const auto& row : rows) {
            std::cout << row.id << " | " << row.category_id << " | "
                      << row.name << " | " << format_beijing_time(row.start_at)
                      << " | " << format_money(row.price_cents) << " | "
                      << (row.capacity - row.occupied) << '/' << row.capacity
                      << " | " << row.status << '\n';
        }
        std::cout << "第 " << filter.page << " 页，" << rows.size() << " 条。\n";
        if (rows.size() == static_cast<std::size_t>(
                std::min(filter.page_size, 100))) {
            std::cout << "可输入 events " << (filter.page + 1)
                      << " 查看下一页。\n";
        }
    } else if (command == "search") {
        SearchFilter filter;
        std::string token;
        while (input >> std::quoted(token)) {
            const auto separator = token.find('=');
            if (separator == std::string::npos) {
                throw BusinessError("查询条件应使用 key=value 格式");
            }
            const auto key = token.substr(0, separator);
            const auto value = token.substr(separator + 1);
            if (key == "category") filter.category_id = std::stoll(value);
            else if (key == "min") filter.min_price_cents = std::stoi(value);
            else if (key == "max") filter.max_price_cents = std::stoi(value);
            else if (key == "from") filter.from_time = parse_beijing_time(value);
            else if (key == "to") filter.to_time = parse_beijing_time(value);
            else if (key == "location") filter.location = value;
            else if (key == "status") filter.status = value;
            else if (key == "order") filter.order_by = value;
            else if (key == "page") filter.page = std::stoi(value);
            else if (key == "size") filter.page_size = std::stoi(value);
            else throw BusinessError("未知查询条件：" + key);
        }
        const auto results = app.search_events(
            filter, session ? std::optional<Id>(session) : std::nullopt);
        for (const auto& e : results) print_event(e);
        std::cout << "查询返回 " << results.size() << " 条。\n";
    } else if (command == "staff") {
        const auto role = word(input, "工作人员身份");
        const auto username = word(input, "用户名");
        const auto phone = word(input, "手机号");
        const auto password = word(input, "临时密码");
        const Id id = app.create_staff(require_login(session), role, username, phone,
                                       password);
        std::cout << "新工作人员 #" << id << '\n';
    } else if (command == "reset") {
        const Id target = read<Id>(input, "用户ID");
        app.reset_password(require_login(session), target, word(input, "临时密码"));
        std::cout << "临时密码已设置，旧密码失效。\n";
    } else if (command == "restrict") {
        const Id target = read<Id>(input, "用户ID");
        const int flag = read<int>(input, "限制标志");
        app.set_restriction(require_login(session), target, flag != 0,
                            word(input, "原因"));
        std::cout << "权限限制已更新。\n";
    } else if (command == "category-add") {
        const Id id = app.add_category(require_login(session), word(input, "名称"));
        std::cout << "类别 #" << id << '\n';
    } else if (command == "category-disable") {
        app.disable_category(require_login(session), read<Id>(input, "类别ID"));
        std::cout << "类别已停用。\n";
    } else if (command == "event-new") {
        const Id id = app.create_event(require_login(session), input_event(input));
        std::cout << "草稿活动 #" << id << '\n';
    } else if (command == "event-edit") {
        const Id id = read<Id>(input, "活动ID");
        app.edit_draft(require_login(session), id, input_event(input));
        std::cout << "草稿已更新。\n";
    } else if (command == "event-desc") {
        const Id id = read<Id>(input, "活动ID");
        app.edit_published_description(require_login(session), id,
                                       word(input, "简介"));
        std::cout << "已发布活动简介已更新。\n";
    } else if (command == "event-submit") {
        app.submit_event(require_login(session), read<Id>(input, "活动ID"));
        std::cout << "活动已提交审核。\n";
    } else if (command == "event-withdraw") {
        app.withdraw_event(require_login(session), read<Id>(input, "活动ID"));
        std::cout << "活动已撤回草稿。\n";
    } else if (command == "event-cancel") {
        app.cancel_event(require_login(session), read<Id>(input, "活动ID"));
        std::cout << "活动已取消，关联订单和票已处理。\n";
    } else if (command == "review") {
        const Id id = read<Id>(input, "活动ID");
        const std::string decision = word(input, "审核决定");
        if (decision != "approve" && decision != "reject") {
            throw BusinessError("审核决定只能是 approve 或 reject");
        }
        app.review_event(require_login(session), id, decision == "approve",
                         word(input, "原因，可用-表示无"));
        std::cout << "审核完成。\n";
    } else if (command == "book") {
        const Id id = read<Id>(input, "活动ID");
        print_booking(app.book(require_login(session), id, word(input, "请求ID")));
    } else if (command == "waitlist") {
        const Id id = read<Id>(input, "活动ID");
        print_booking(app.join_waitlist(require_login(session), id,
                                        word(input, "请求ID")));
    } else if (command == "pay") {
        const Id id = read<Id>(input, "报名ID");
        print_booking(app.pay(require_login(session), id, word(input, "请求ID")));
    } else if (command == "cancel") {
        const Id id = read<Id>(input, "报名ID");
        app.cancel_booking(require_login(session), id, word(input, "请求ID"));
        std::cout << "取消或模拟退款完成。\n";
    } else if (command == "checkin") {
        const Id id = read<Id>(input, "活动ID");
        const std::string code = word(input, "票码");
        app.check_in(require_login(session), id, code, word(input, "请求ID"));
        std::cout << "验票签到成功。\n";
    } else if (command == "report") {
        const Id id = read<Id>(input, "活动ID");
        const Id report_id = app.report_event(require_login(session), id,
                                              word(input, "举报原因"));
        std::cout << "举报 #" << report_id << '\n';
    } else if (command == "decide") {
        const Id id = read<Id>(input, "举报ID");
        const std::string choice = word(input, "true或false");
        if (choice != "true" && choice != "false") {
            throw BusinessError("核查结论只能是 true 或 false");
        }
        app.decide_report(require_login(session), id, choice == "true",
                          word(input, "处理原因"));
        std::cout << "举报核查完成。\n";
    } else if (command == "mine") {
        const auto records = app.my_bookings(require_login(session));
        for (const auto& item : records) print_booking(item);
        std::cout << "共 " << records.size() << " 条报名记录。\n";
    } else if (command == "notices") {
        const auto records = app.notices(require_login(session));
        for (const auto& message : records) std::cout << "- " << message << '\n';
    } else if (command == "stats") {
        print_stats(app.statistics(require_login(session),
                                   read<Id>(input, "活动ID")));
    } else if (command == "list") {
        const Id id = read<Id>(input, "活动ID");
        for (const auto& row : app.participant_list(require_login(session), id)) {
            std::cout << row.username << "  " << row.ticket_code
                      << "  " << row.status << '\n';
        }
    } else if (command == "risk") {
        const Id id = read<Id>(input, "用户ID");
        const auto counts = app.risk_counts(require_login(session), id);
        std::cout << "近30天缺席 " << counts.absent << "；主动退票 "
                  << counts.voluntary_refunds << "；组织者违规 "
                  << counts.organizer_violations << "；待管理员处理 "
                  << (counts.pending_admin_review ? "是" : "否") << '\n';
    } else if (command == "export") {
        const Id id = read<Id>(input, "活动ID");
        const auto kind = word(input, "导出种类");
        const auto file = word(input, "文件路径");
        app.export_csv(require_login(session), id, file, kind);
        std::cout << "已导出 " << file << '\n';
    } else if (command == "audit") {
        const Id id = read<Id>(input, "活动ID或0");
        for (const auto& entry : app.audit(require_login(session), id)) {
            std::cout << '#' << entry.id << ' ' << entry.action << ' '
                      << entry.before_value << " -> " << entry.after_value
                      << " @ " << format_beijing_time(entry.created_at) << '\n';
        }
    } else if (command == "maintenance") {
        app.maintenance();
        std::cout << "到期订单和缺席记录处理完成。\n";
    } else {
        throw BusinessError("未知命令。输入 help 查看可用命令。");
    }
    return true;
}

}  // namespace
}  // namespace ticket

int main(int argc, char** argv) {
#ifdef _WIN32
    // MinGW emits UTF-8 source strings; match the Windows console code page.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::filesystem::path database = "ticket.db";
    std::string script;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--db" && index + 1 < argc) {
            database = argv[++index];
        } else if (argument == "--script" && index + 1 < argc) {
            script = argv[++index];
        } else {
            std::cerr << "用法: ticket_app [--db 文件] [--script 命令文件]\n";
            return 2;
        }
    }
    try {
        ticket::App app(database);
        app.maintenance();
        ticket::Id session = 0;
        std::cout << "活动票务与报名系统已启动。输入 help 查看命令。\n";
        if (!script.empty()) {
            std::ifstream input(script);
            if (!input) throw ticket::BusinessError("无法打开命令脚本");
            std::string line;
            while (std::getline(input, line)) {
                std::cout << "> " << line << '\n';
                try {
                    if (!ticket::execute_line(app, session, line)) break;
                } catch (const std::exception& error) {
                    std::cout << "失败：" << error.what() << '\n';
                }
            }
        } else {
            std::string line;
            while (std::cout << "> " && std::getline(std::cin, line)) {
                try {
                    if (!ticket::execute_line(app, session, line)) break;
                } catch (const std::exception& error) {
                    std::cout << "失败：" << error.what() << '\n';
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "启动失败：" << error.what() << '\n';
        return 1;
    }
    return 0;
}
