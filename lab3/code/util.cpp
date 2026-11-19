#include "app.hpp"

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace ticket {

BusinessError::BusinessError(const std::string& message)
    : std::runtime_error(message) {}

Time parse_beijing_time(const std::string& value) {
    // 固定解释为北京时间，避免运行计算机的时区改变测试结果。
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d",
                    &year, &month, &day, &hour, &minute) != 5) {
        throw BusinessError("时间格式应为 YYYY-MM-DDTHH:MM");
    }
    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        throw BusinessError("时间字段越界");
    }
    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
#ifdef _WIN32
    const Time utc = _mkgmtime(&parts);
#else
    const Time utc = timegm(&parts);
#endif
    if (utc < 0) {
        throw BusinessError("无效时间");
    }
    return utc - 8 * 3600;
}

std::string format_beijing_time(Time value) {
    const std::time_t adjusted = value + 8 * 3600;
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &adjusted);
#else
    gmtime_r(&adjusted, &parts);
#endif
    std::ostringstream output;
    output << std::put_time(&parts, "%Y-%m-%d %H:%M");
    return output.str();
}

std::string format_money(int cents) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << (cents / 100.0);
    return output.str();
}

std::string csv_escape(const std::string& value) {
    std::string result = "\"";
    for (char ch : value) {
        if (ch == '"') {
            result += "\"\"";
        } else {
            result += ch;
        }
    }
    result += '"';
    return result;
}

int utf8_characters(const std::string& value) {
    int count = 0;
    for (unsigned char ch : value) {
        if ((ch & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

}  // namespace ticket
