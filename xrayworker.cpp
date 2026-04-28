#include "XRayWorker.h"

#include <QThread>

namespace
{
constexpr int kInitialConnectTimeoutMs = 400;
constexpr int kRetryConnectTimeoutMs = 500;
constexpr int kRetryDelayMs = 500;
constexpr int kFirstRetryDelayMs = 800;
constexpr int kConnectRetryCount = 8;
constexpr int kPollIntervalMs = 1000;
}

XRayWorker::XRayWorker(QObject* parent)
    : QObject(parent),
      m_controller(nullptr),
      m_pollTimer(new QTimer(this))
{
    qRegisterMetaType<XRayController::Status>("XRayController::Status");

    m_pollTimer->setInterval(kPollIntervalMs);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &XRayWorker::pollStatus);
}

XRayWorker::~XRayWorker()
{
    stopPolling();
    cleanupController(true);
}

void XRayWorker::emitControllerError()
{
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("XRayController 未初始化"));
        return;
    }

    const QString error = m_controller->lastError();
    if (!error.isEmpty()) {
        emit errorOccurred(error);
    }
}

void XRayWorker::cleanupController(bool stopVendorSoftware)
{
    if (!m_controller) {
        return;
    }

    if (m_controller->isConnected()) {
        m_controller->disconnectFromDevice();
    }

    if (stopVendorSoftware) {
        m_controller->stopVendorSoftware();
    }

    m_controller.reset();
}

void XRayWorker::startPolling()
{
    if (m_pollTimer && !m_pollTimer->isActive()) {
        m_pollTimer->start();
    }
}

void XRayWorker::stopPolling()
{
    if (m_pollTimer && m_pollTimer->isActive()) {
        m_pollTimer->stop();
    }
}

bool XRayWorker::connectWithAutoStart()
{
    if (!m_controller) {
        return false;
    }

    /* 先走直连快路径；只有端口不可达时，才尝试拉起厂商软件。 */
    if (m_controller->connectToDevice(m_host, m_port, kInitialConnectTimeoutMs)) {
        return true;
    }

    const QString trimmedExePath = m_vendorExePath.trimmed();
    if (trimmedExePath.isEmpty()) {
        return false;
    }

    if (!m_controller->startVendorSoftware(trimmedExePath, m_vendorArgs, m_workingDirectory)) {
        return false;
    }

    /* 厂商进程启动后通常要等一小段时间，控制端口才会开始监听。 */
    for (int attempt = 0; attempt < kConnectRetryCount; ++attempt) {
        QThread::msleep(attempt == 0 ? kFirstRetryDelayMs : kRetryDelayMs);
        if (m_controller->connectToDevice(m_host, m_port, kRetryConnectTimeoutMs)) {
            return true;
        }
    }

    return false;
}

void XRayWorker::publishCurrentStatus()
{
    if (!m_controller) {
        return;
    }

    emit statusUpdated(m_controller->lastStatus());
    emit xrayStateChanged(m_controller->isHighVoltageOn());
}

void XRayWorker::initialize(const QString& vendorExePath,
                            const QStringList& vendorArgs,
                            const QString& workingDirectory,
                            const QString& host,
                            const quint16& port)
{
    /* 每次重连都从干净状态重新建控制器，避免沿用上一轮 socket/process 残留。 */
    m_vendorExePath = vendorExePath;
    m_vendorArgs = vendorArgs;
    m_workingDirectory = workingDirectory;
    m_host = host;
    m_port = port;

    stopPolling();
    cleanupController(true);
    m_controller = std::make_unique<XRayController>();

    if (!connectWithAutoStart()) {
        emitControllerError();
        cleanupController(true);
        emit connectionChanged(false);
        emit initialized(false);
        return;
    }

    emit connectionChanged(true);

    if (!m_controller->queryStatus()) {
        emitControllerError();
        cleanupController(true);
        emit connectionChanged(false);
        emit initialized(false);
        return;
    }

    publishCurrentStatus();
    startPolling();
    emit initialized(true);
}

void XRayWorker::shutdown()
{
    stopPolling();

    if (m_controller && m_controller->isConnected()) {
        m_controller->powerOff();
    }

    cleanupController(true);
    emit xrayStateChanged(false);
    emit connectionChanged(false);
}

void XRayWorker::openXRay()
{
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("XRayController 未初始化"));
        return;
    }

    if (!m_controller->powerOn()) {
        emitControllerError();
        return;
    }

    publishCurrentStatus();
}

void XRayWorker::closeXRay()
{
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("XRayController 未初始化"));
        return;
    }

    if (!m_controller->powerOff()) {
        emitControllerError();
        return;
    }

    publishCurrentStatus();
}

void XRayWorker::applySettings(double kv, int ua)
{
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("XRayController 未初始化"));
        return;
    }

    if (!m_controller->setParameters(kv, ua)) {
        emitControllerError();
        return;
    }

    publishCurrentStatus();
}

void XRayWorker::queryStatus()
{
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("XRayController 未初始化"));
        return;
    }

    if (!m_controller->queryStatus()) {
        emitControllerError();
        return;
    }

    publishCurrentStatus();
}

void XRayWorker::pollStatus()
{
    if (!m_controller) {
        return;
    }

    /* 轮询线程同时承担“保活”职责，任何链路故障都立即回退到断开状态。 */
    if (!m_controller->queryStatus()) {
        const QString error = m_controller->lastError();

        stopPolling();
        m_controller->disconnectFromDevice();
        emit xrayStateChanged(false);
        emit connectionChanged(false);

        if (!error.isEmpty()) {
            emit errorOccurred(error);
        }
        return;
    }

    publishCurrentStatus();
}
