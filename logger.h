#ifndef LOGGER_H
#define LOGGER_H

#include <QString>

class Logger
{
public:
    // 初始化日志系统：把 Qt 日志写入 logFilePath（如 "XXX.log" 或绝对路径）
    static bool init(const QString& logFilePath, bool alsoPrintToStderr = true);

    // 可选：主动关闭（一般不必调用）
    static void shutdown();
};

#endif // LOGGER_H
