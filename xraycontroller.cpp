#include "XRayController.h"

#include <QDebug>
#include <QFileInfo>
#include <QVector>
#include <QtGlobal>

namespace
{
constexpr int kDefaultQueryTimeoutMs = 1500;
constexpr int kExpectedReplyFieldCount = 15;
constexpr auto kReplyHeader = "C2HS0019";
}

XRayController::XRayController()
    : m_process(std::make_unique<QProcess>()),
      m_socket(std::make_unique<QTcpSocket>())
{
    QObject::connect(
        m_process.get(),
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        [this](int, QProcess::ExitStatus) {
            m_vendorStartedByUs = false;
        });
}

XRayController::~XRayController()
{
    disconnectFromDevice();
    stopVendorSoftware();
}

void XRayController::setError(const QString& msg)
{
    m_lastError = msg;
    qWarning() << "[XRayController]" << msg;
}

QString XRayController::lastError() const
{
    return m_lastError;
}

bool XRayController::startVendorSoftware(const QString& program,
                                         const QStringList& arguments,
                                         const QString& workingDirectory,
                                         int startTimeoutMs)
{
    /* 某些现场环境必须先启动厂商 EXE，控制端口才会真正开放。 */
    m_lastError.clear();

    const QString trimmedProgram = program.trimmed();
    if (trimmedProgram.isEmpty()) {
        setError(QStringLiteral("厂商软件路径为空"));
        return false;
    }

    const QFileInfo fileInfo(trimmedProgram);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        setError(QStringLiteral("厂商软件不存在: %1").arg(trimmedProgram));
        return false;
    }

    if (m_process->state() != QProcess::NotRunning) {
        qInfo() << "[XRayController] vendor software already started by this process.";
        return true;
    }

    if (!workingDirectory.isEmpty()) {
        m_process->setWorkingDirectory(workingDirectory);
    } else {
        m_process->setWorkingDirectory(fileInfo.absolutePath());
    }

    m_process->start(trimmedProgram, arguments);

    if (!m_process->waitForStarted(startTimeoutMs)) {
        m_vendorStartedByUs = false;
        setError(QStringLiteral("启动厂商软件失败: %1").arg(m_process->errorString()));
        return false;
    }

    m_vendorStartedByUs = true;
    qInfo() << "[XRayController] vendor software started:" << trimmedProgram;
    return true;
}

void XRayController::stopVendorSoftware(int timeoutMs)
{
    if (!m_process || !m_vendorStartedByUs) {
        return;
    }

    if (m_process->state() == QProcess::NotRunning) {
        m_vendorStartedByUs = false;
        return;
    }

    m_process->terminate();
    if (!m_process->waitForFinished(timeoutMs)) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }

    m_vendorStartedByUs = false;
    qInfo() << "[XRayController] vendor software stopped.";
}

bool XRayController::connectToDevice(const QString& host, quint16 port, int timeoutMs)
{
    /* 传输层就是一条普通 TCP 连接，既可以连本机 EXE，也可以连远端控制端口。 */
    m_lastError.clear();

    if (!m_socket) {
        setError(QStringLiteral("内部 socket 未初始化"));
        return false;
    }

    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        return true;
    }

    m_socket->abort();
    m_socket->connectToHost(host, port);

    if (!m_socket->waitForConnected(timeoutMs)) {
        setError(QStringLiteral("连接光源失败: %1").arg(m_socket->errorString()));
        return false;
    }

    qInfo() << "[XRayController] connected to" << host << port;
    return true;
}

void XRayController::disconnectFromDevice()
{
    if (!m_socket) {
        return;
    }

    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->waitForDisconnected(1000);
        }
    }

    m_lastStatus = Status{};
    qInfo() << "[XRayController] disconnected.";
}

bool XRayController::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool XRayController::isHighVoltageOn() const
{
    return m_lastStatus.valid && m_lastStatus.faceState == 0x0004;
}

bool XRayController::validateVoltageDeciKv(int kvDeci)
{
    if (kvDeci < 0 || kvDeci > 9999) {
        setError(QStringLiteral("管电压超出范围，允许范围为 0~999.9 kV"));
        return false;
    }

    return true;
}

bool XRayController::validateCurrentUa(int ua)
{
    if (ua < 0 || ua > 999999) {
        setError(QStringLiteral("管电流超出范围，允许范围为 0~999999 uA"));
        return false;
    }

    return true;
}

bool XRayController::setParameters(double kv, int ua)
{
    const int kvDeci = qRound(kv * 10.0);

    if (!validateVoltageDeciKv(kvDeci) || !validateCurrentUa(ua)) {
        return false;
    }

    m_setKvDeci = kvDeci;
    m_setUa = ua;
    return sendCurrentCommand(isHighVoltageOn());
}

bool XRayController::setVoltageKv(double kv)
{
    return setVoltageDeciKv(qRound(kv * 10.0));
}

bool XRayController::setVoltageDeciKv(int kvDeci)
{
    if (!validateVoltageDeciKv(kvDeci)) {
        return false;
    }

    m_setKvDeci = kvDeci;
    return sendCurrentCommand(isHighVoltageOn());
}

bool XRayController::setCurrentUa(int ua)
{
    if (!validateCurrentUa(ua)) {
        return false;
    }

    m_setUa = ua;
    return sendCurrentCommand(isHighVoltageOn());
}

bool XRayController::powerOn()
{
    return sendCurrentCommand(true);
}

bool XRayController::powerOff()
{
    return sendCurrentCommand(false);
}

QByteArray XRayController::buildCommandFrame(bool hvOn) const
{
    /*
     * 这个协议把设定电压、电流和高压开关状态都塞进同一条 ASCII 帧里。
     * 因此“查询状态”本质上也是重发当前目标参数，再解析设备回显。
     */
    const QString header = QStringLiteral("H2CS0003");
    const QString kvStr = QStringLiteral("%1").arg(m_setKvDeci, 4, 10, QLatin1Char('0'));
    const QString uaStr = QStringLiteral("%1").arg(m_setUa, 6, 10, QLatin1Char('0'));
    const QString reserve = QStringLiteral("0");
    const QString workType = QStringLiteral("A");
    const QString hvChar = hvOn ? QStringLiteral("P") : QStringLiteral("T");
    const QString mode = QStringLiteral("0");
    const QString reset = QStringLiteral("N");
    const QString targetUa = QStringLiteral("0000");
    const QString startKv = QStringLiteral("0000");

    const QString frame = header + kvStr + uaStr + reserve + workType
                          + hvChar + mode + reset + targetUa + startKv
                          + QStringLiteral("\r\n");

    return frame.toLatin1();
}

bool XRayController::sendFrameAndReadReply(const QByteArray& frame, QByteArray* replyOut)
{
    m_lastError.clear();

    if (!isConnected()) {
        setError(QStringLiteral("光源未连接"));
        return false;
    }

    /* 先清空上一次残留回复，保证本次收发是一一对应的事务。 */
    m_socket->readAll();

    qInfo().noquote() << "[XRayController] send:" << QString::fromLatin1(frame).trimmed();

    const qint64 written = m_socket->write(frame);
    if (written != frame.size()) {
        setError(QStringLiteral("命令发送失败，写入字节数异常: %1/%2")
                     .arg(written)
                     .arg(frame.size()));
        return false;
    }

    if (!m_socket->waitForBytesWritten(1000)) {
        setError(QStringLiteral("等待发送完成失败: %1").arg(m_socket->errorString()));
        return false;
    }

    if (!m_socket->waitForReadyRead(kDefaultQueryTimeoutMs)) {
        setError(QStringLiteral("等待设备应答超时: %1").arg(m_socket->errorString()));
        return false;
    }

    QByteArray reply = m_socket->readAll();
    while (m_socket->waitForReadyRead(30)) {
        reply += m_socket->readAll();
    }

    if (replyOut) {
        *replyOut = reply;
    }

    qInfo().noquote() << "[XRayController] reply:" << QString::fromLatin1(reply).trimmed();
    return true;
}

bool XRayController::parseReply(const QByteArray& reply, Status* statusOut)
{
    /* 回复格式固定为“头 + 15 个十六进制字段”，逗号和空格都视为分隔符。 */
    const QString text = QString::fromLatin1(reply).trimmed();

    if (!text.startsWith(QLatin1String(kReplyHeader))) {
        setError(QStringLiteral("非法应答帧: %1").arg(text));
        return false;
    }

    QString payload = text.mid(8).trimmed();
    payload.replace(',', ' ');
    const QString normalized = payload.simplified();
    const QStringList parts = normalized.split(' ', Qt::SkipEmptyParts);

    if (parts.size() < kExpectedReplyFieldCount) {
        setError(QStringLiteral("应答字段不足: %1").arg(text));
        return false;
    }

    const QStringList fieldNames = {
        QStringLiteral("电脑故障码"),
        QStringLiteral("控制器故障码"),
        QStringLiteral("高压状态"),
        QStringLiteral("系统模式"),
        QStringLiteral("接口板位数据"),
        QStringLiteral("控制板位数据"),
        QStringLiteral("设定管压"),
        QStringLiteral("设定管流"),
        QStringLiteral("实测靶电流"),
        QStringLiteral("曝光时间"),
        QStringLiteral("实测管压"),
        QStringLiteral("实测管流"),
        QStringLiteral("流量反馈"),
        QStringLiteral("放电计数"),
        QStringLiteral("保护触发器")
    };

    QVector<int> values;
    values.reserve(kExpectedReplyFieldCount);

    for (int index = 0; index < kExpectedReplyFieldCount; ++index) {
        bool ok = false;
        const int value = parts.at(index).toInt(&ok, 16);
        if (!ok) {
            setError(QStringLiteral("解析%1失败: %2")
                         .arg(fieldNames.at(index))
                         .arg(parts.at(index)));
            return false;
        }
        values.push_back(value);
    }

    /* 协议里大部分字段本质上是控制器寄存器，先原样保留，解释逻辑留给上层。 */
    Status status;
    status.valid = true;
    status.pcFaultCode = values.at(0);
    status.controllerFaultCode = values.at(1);
    status.faceState = values.at(2);
    status.systemMode = values.at(3);
    status.faceBitData = values.at(4);
    status.bitData = values.at(5);
    status.setKvDeci = values.at(6);
    status.setMaCenti = values.at(7);
    status.readTargetUa = values.at(8);
    status.readTimeSec = values.at(9);
    status.measuredKvDeci = values.at(10);
    status.measuredMaCenti = values.at(11);
    status.flow = values.at(12);
    status.arcCounter = values.at(13);
    status.trigger = values.at(14);

    if (status.pcFaultCode != 0 || status.controllerFaultCode != 0) {
        qWarning() << "[XRayController] fault state reported by device:"
                   << "pcFault =" << status.pcFaultCode
                   << "controllerFault =" << status.controllerFaultCode;
    }

    if (statusOut) {
        *statusOut = status;
    }

    return true;
}

void XRayController::applyStatus(const Status& status)
{
    /* 用设备回显覆盖本地缓存，保证后续查询/改参都基于控制器已接受的值。 */
    m_lastStatus = status;
    m_setKvDeci = status.setKvDeci;
    m_setUa = qRound(static_cast<double>(status.setMaCenti) * 10.0);
}

bool XRayController::sendCurrentCommand(bool hvOn)
{
    QByteArray reply;
    const QByteArray frame = buildCommandFrame(hvOn);

    if (!sendFrameAndReadReply(frame, &reply)) {
        return false;
    }

    Status status;
    if (!parseReply(reply, &status)) {
        return false;
    }

    applyStatus(status);
    return true;
}

bool XRayController::queryStatus()
{
    /* 当前协议没有独立的“被动查询帧”，因此复发当前命令帧来拿状态快照。 */
    QByteArray reply;
    const QByteArray frame = buildCommandFrame(isHighVoltageOn());

    if (!sendFrameAndReadReply(frame, &reply)) {
        return false;
    }

    Status status;
    if (!parseReply(reply, &status)) {
        return false;
    }

    applyStatus(status);
    return true;
}

XRayController::Status XRayController::lastStatus() const
{
    return m_lastStatus;
}
