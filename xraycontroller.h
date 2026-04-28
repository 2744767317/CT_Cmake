#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QProcess>
#include <QString>
#include <QTcpSocket>

#include <memory>

/*
 * XRayController 封装 X 射线源厂商软件进程和 TCP 指令协议。
 * 它负责：
 * 1. 启停厂商 EXE；
 * 2. 维护与控制端口的 TCP 连接；
 * 3. 组装命令帧、解析状态帧，并缓存最新状态。
 *
 * 线程上由 XRayWorker 独占使用，MainWindow 不直接接触 socket/process。
 */
class XRayController
{
public:
    /* 把固定格式十六进制状态帧解析成便于界面和安全检查使用的结构。 */
    struct Status
    {
        bool valid = false;

        int pcFaultCode = 0;
        int controllerFaultCode = 0;
        int faceState = -1;
        int systemMode = -1;
        int faceBitData = 0;
        int bitData = 0;

        int setKvDeci = 0;        // 0.1 kV
        int setMaCenti = 0;       // 0.01 mA
        int readTargetUa = 0;     // uA
        int readTimeSec = 0;      // s
        int measuredKvDeci = 0;   // 0.1 kV
        int measuredMaCenti = 0;  // 0.01 mA
        int flow = 0;
        int arcCounter = 0;
        int trigger = 0;
    };

public:
    XRayController();
    ~XRayController();

    bool startVendorSoftware(const QString& program,
                             const QStringList& arguments = {},
                             const QString& workingDirectory = QString(),
                             int startTimeoutMs = 5000);

    void stopVendorSoftware(int timeoutMs = 3000);

    bool connectToDevice(const QString& host = "127.0.0.1",
                         quint16 port = 3600,
                         int timeoutMs = 3000);

    void disconnectFromDevice();

    bool isConnected() const;
    bool isHighVoltageOn() const;

    bool setParameters(double kv, int ua);
    bool setVoltageKv(double kv);
    bool setVoltageDeciKv(int kvDeci);
    bool setCurrentUa(int ua);

    bool powerOn();
    bool powerOff();

    /* 查询最新状态。当前协议实现里复用“当前命令帧”作为主动轮询请求。 */
    bool queryStatus();
    Status lastStatus() const;

    QString lastError() const;

private:
    /* 组装厂商软件要求的 ASCII 控制帧。 */
    QByteArray buildCommandFrame(bool hvOn) const;
    bool sendCurrentCommand(bool hvOn);
    /* 发送一帧并同步等待对应回复。 */
    bool sendFrameAndReadReply(const QByteArray& frame, QByteArray* replyOut);
    /* 校验回复头，并解析 15 个十六进制字段。 */
    bool parseReply(const QByteArray& reply, Status* statusOut);
    bool validateVoltageDeciKv(int kvDeci);
    bool validateCurrentUa(int ua);
    void applyStatus(const Status& status);

    void setError(const QString& msg);

private:
    std::unique_ptr<QProcess> m_process;
    std::unique_ptr<QTcpSocket> m_socket;

    int m_setKvDeci = 0;  // 0.1 kV
    int m_setUa = 0;      // uA
    bool m_vendorStartedByUs = false;
    QString m_lastError;

    Status m_lastStatus;
};

Q_DECLARE_METATYPE(XRayController::Status)
