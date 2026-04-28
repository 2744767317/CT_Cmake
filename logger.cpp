#include "logger.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QCoreApplication>
#include <QtGlobal>

namespace {

QFile*        g_file = nullptr;
QTextStream*  g_stream = nullptr;
QMutex        g_mutex;
bool          g_alsoStderr = true;

static inline const char* levelToString(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
#if (QT_VERSION >= QT_VERSION_CHECK(5, 5, 0))
    case QtInfoMsg:     return "INFO";
#endif
    case QtWarningMsg:  return "WARN";
    case QtCriticalMsg: return "ERROR";
    case QtFatalMsg:    return "FATAL";
    default:            return "LOG";
    }
}

static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    // 时间戳（本地时间）
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const quintptr tid = reinterpret_cast<quintptr>(QThread::currentThreadId());

    // ctx.file 可能为空（取决于编译选项/调用方式）
    const char* file = ctx.file ? ctx.file : "";
    const char* func = ctx.function ? ctx.function : "";
    const int line = ctx.line;

    // 组装一行日志
    QString lineStr = QString("[%1] [%2] [T%3] %4:%5 (%6) - %7")
                          .arg(ts)
                          .arg(levelToString(type))
                          .arg(QString::number(tid, 16))
                          .arg(QString::fromUtf8(file))
                          .arg(line)
                          .arg(QString::fromUtf8(func))
                          .arg(msg);

    // qFatal 结束行更清晰
    lineStr.append('\n');

    {
        QMutexLocker locker(&g_mutex);

        if (g_stream) {
            (*g_stream) << lineStr;
            g_stream->flush();      // 关键：实时写入
            if (g_file) g_file->flush();
        }
    }

    // 同步输出到 stderr（可选）
    if (g_alsoStderr) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 4, 0))
        fprintf(stderr, "%s", lineStr.toLocal8Bit().constData());
#else
        fprintf(stderr, "%s", lineStr.toUtf8().constData());
#endif
        fflush(stderr);
    }

    // QtFatalMsg 必须终止
    if (type == QtFatalMsg) {
        abort();
    }
}

} // namespace

bool Logger::init(const QString& logFilePath, bool alsoPrintToStderr)
{
    QMutexLocker locker(&g_mutex);

    g_alsoStderr = alsoPrintToStderr;

    // 重复 init 先关闭旧的
    if (g_stream) { delete g_stream; g_stream = nullptr; }
    if (g_file)   { g_file->close(); delete g_file; g_file = nullptr; }

    g_file = new QFile(logFilePath);
    // 追加写入 + 文本模式
    if (!g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete g_file;
        g_file = nullptr;
        return false;
    }

    g_stream = new QTextStream(g_file);
    g_stream->setEncoding(QStringConverter::Encoding::Utf8);

    // 可选：写一条启动标记
    const QString header = QString("\n===== LOG START %1 | app=%2 =====\n")
                               .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                               .arg(QCoreApplication::applicationName());
    (*g_stream) << header;
    g_stream->flush();
    g_file->flush();

    qInstallMessageHandler(messageHandler);
    return true;
}

void Logger::shutdown()
{
    QMutexLocker locker(&g_mutex);

    // 还原默认 handler
    qInstallMessageHandler(nullptr);

    if (g_stream) { g_stream->flush(); delete g_stream; g_stream = nullptr; }
    if (g_file)   { g_file->flush(); g_file->close(); delete g_file; g_file = nullptr; }
}
