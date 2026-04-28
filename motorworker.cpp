#include "MotorWorker.h"
#include "MotorController.h"

#include <QDebug>
#include <QThread>

MotorWorker::MotorWorker(QObject* parent)
    : QObject(parent)
{
}

MotorWorker::~MotorWorker()
{
    shutdown();
}

void MotorWorker::initialize()
{
    if (!m_controller) {
        /* controller 和轮询定时器都在 worker 线程内创建，避免跨线程 QObject 归属问题。 */
        m_controller = std::make_unique<MotorController>();
    }

    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setTimerType(Qt::CoarseTimer);

        connect(m_pollTimer, &QTimer::timeout,
                this, &MotorWorker::pollStatusOnce);
    }

    qInfo() << "[MotorWorker] initialized, thread =" << QThread::currentThread();
}

void MotorWorker::shutdown()
{
    if (m_pollTimer && m_pollTimer->isActive()) {
        m_pollTimer->stop();
        qInfo() << "[MotorWorker] polling stopped in shutdown";
    }

    if (m_controller) {
        m_controller->disconnectController();
        m_controller.reset();
    }

    qInfo() << "[MotorWorker] shutdown";
}

void MotorWorker::emitUiError(const QString& title, const QString& message)
{
    emit errorOccurred(title, message);
}

void MotorWorker::notifyIfFailed(const QString& opName, bool ok, const QString& fallbackMessage)
{
    if (ok) {
        return;
    }

    /* 如果 DLL 已经给了更细的错误文本，优先直接透传给界面。 */
    QString detail = fallbackMessage;
    if (m_controller) {
        const QString lastErr = m_controller->lastError();
        if (!lastErr.isEmpty()) {
            detail = lastErr;
        }
    }

    emitUiError(opName, detail);
}

void MotorWorker::connectSerial(quint8 comPort)
{
    if (!m_controller) {
        initialize();
    }

    /* 连接成功后立即开始轮询，让 UI 尽快拿到第一帧真实轴状态。 */
    qInfo() << "[MotorWorker] connectSerial, COM =" << comPort;
    const bool ok = m_controller->connectSerial(comPort);

    if (ok) {
        startPolling();
        pollStatusOnce();
    } else {
        stopPolling();
    }

    emit connectedChanged(ok);
    emit operationFinished("connectSerial", ok,
                           ok ? QString("Connected to COM%1.").arg(comPort)
                              : QString("Failed to connect to COM%1.").arg(comPort));

    notifyIfFailed("Motor Connection Error",
                   ok,
                   QString("Failed to connect to COM%1.").arg(comPort));
}

void MotorWorker::connectNetwork(const QString& ip, qint32 port)
{
    if (!m_controller) {
        initialize();
    }

    /* 网络路径与串口路径保持同构，方便后续界面无缝切换。 */
    qInfo() << "[MotorWorker] connectNetwork, ip =" << ip << ", port =" << port;
    const bool ok = m_controller->connectNetwork(ip, port);

    if (ok) {
        startPolling();
        pollStatusOnce();
    } else {
        stopPolling();
    }

    emit connectedChanged(ok);
    emit operationFinished("connectNetwork", ok,
                           ok ? QString("Connected to %1:%2.").arg(ip).arg(port)
                              : QString("Failed to connect to %1:%2.").arg(ip).arg(port));

    notifyIfFailed("Motor Connection Error",
                   ok,
                   QString("Failed to connect to %1:%2.").arg(ip).arg(port));
}

void MotorWorker::disconnectController()
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] disconnectController ignored: controller is null";
        return;
    }

    stopPolling();

    qInfo() << "[MotorWorker] disconnectController";
    const bool ok = m_controller->disconnectController();

    emit connectedChanged(false);
    emit operationFinished("disconnectController", ok,
                           ok ? "Disconnected."
                              : "Failed to disconnect controller.");

    notifyIfFailed("Motor Disconnection Error",
                   ok,
                   "Failed to disconnect controller.");
}

void MotorWorker::absMove(quint8 axisId, float pos, float vel, float acc)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] absMove ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->absMove(axisId, pos, vel, acc);

    emit operationFinished("absMove", ok,
                           ok ? QString("Axis %1 absolute move succeeded.").arg(axisId)
                              : QString("Axis %1 absolute move failed.").arg(axisId));

    notifyIfFailed("Motor Move Error",
                   ok,
                   QString("Axis %1 absolute move failed.").arg(axisId));
}

void MotorWorker::relMove(quint8 axisId, float dist, float vel, float acc)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] relMove ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->relMove(axisId, dist, vel, acc);

    emit operationFinished("relMove", ok,
                           ok ? QString("Axis %1 relative move succeeded.").arg(axisId)
                              : QString("Axis %1 relative move failed.").arg(axisId));

    notifyIfFailed("Motor Move Error",
                   ok,
                   QString("Axis %1 relative move failed.").arg(axisId));
}

void MotorWorker::moveAtSpeed(quint8 axisId, float vel, float acc)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] moveAtSpeed ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->moveAtSpeed(axisId, vel, acc);

    emit operationFinished("moveAtSpeed", ok,
                           ok ? QString("Axis %1 move-at-speed succeeded.").arg(axisId)
                              : QString("Axis %1 move-at-speed failed.").arg(axisId));

    notifyIfFailed("Motor Move Error",
                   ok,
                   QString("Axis %1 move-at-speed failed.").arg(axisId));
}

void MotorWorker::stopAxis(quint8 axisId, float dec)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] stopAxis ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->stopAxis(axisId, dec);

    emit operationFinished("stopAxis", ok,
                           ok ? QString("Axis %1 stop succeeded.").arg(axisId)
                              : QString("Axis %1 stop failed.").arg(axisId));

    notifyIfFailed("Motor Stop Error",
                   ok,
                   QString("Axis %1 stop failed.").arg(axisId));
}

void MotorWorker::emergencyStopAxis(quint8 axisId)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] emergencyStopAxis ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->emergencyStopAxis(axisId);

    emit operationFinished("emergencyStopAxis", ok,
                           ok ? QString("Axis %1 emergency stop succeeded.").arg(axisId)
                              : QString("Axis %1 emergency stop failed.").arg(axisId));

    notifyIfFailed("Motor Emergency Stop Error",
                   ok,
                   QString("Axis %1 emergency stop failed.").arg(axisId));
}

void MotorWorker::pauseAxis(quint8 axisId)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] pauseAxis ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->pauseAxis(axisId);

    emit operationFinished("pauseAxis", ok,
                           ok ? QString("Axis %1 pause succeeded.").arg(axisId)
                              : QString("Axis %1 pause failed.").arg(axisId));

    notifyIfFailed("Motor Pause Error",
                   ok,
                   QString("Axis %1 pause failed.").arg(axisId));
}

void MotorWorker::restartAxis(quint8 axisId)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] restartAxis ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->restartAxis(axisId);

    emit operationFinished("restartAxis", ok,
                           ok ? QString("Axis %1 restart succeeded.").arg(axisId)
                              : QString("Axis %1 restart failed.").arg(axisId));

    notifyIfFailed("Motor Restart Error",
                   ok,
                   QString("Axis %1 restart failed.").arg(axisId));
}

void MotorWorker::seekZero(quint8 axisId, float vel, float acc)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] seekZero ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->seekZero(axisId, vel, acc);

    emit operationFinished("seekZero", ok,
                           ok ? QString("Axis %1 seek-zero succeeded.").arg(axisId)
                              : QString("Axis %1 seek-zero failed.").arg(axisId));

    notifyIfFailed("Motor Homing Error",
                   ok,
                   QString("Axis %1 seek-zero failed.").arg(axisId));
}

void MotorWorker::cancelSeekZero(quint8 axisId)
{
    if (!m_controller) {
        qWarning() << "[MotorWorker] cancelSeekZero ignored: controller is null";
        emitUiError("Motor Error", "Motor controller is not initialized.");
        return;
    }

    const bool ok = m_controller->cancelSeekZero(axisId);

    emit operationFinished("cancelSeekZero", ok,
                           ok ? QString("Axis %1 cancel seek-zero succeeded.").arg(axisId)
                              : QString("Axis %1 cancel seek-zero failed.").arg(axisId));

    notifyIfFailed("Motor Homing Error",
                   ok,
                   QString("Axis %1 cancel seek-zero failed.").arg(axisId));
}

void MotorWorker::startPolling(int intervalMs)
{
    if (!m_controller) {
        initialize();
    }

    if (!m_pollTimer) {
        qWarning() << "[MotorWorker] startPolling ignored: timer is null";
        return;
    }

    if (intervalMs < 50) {
        intervalMs = 50;
    }

    m_pollTimer->start(intervalMs);
    qInfo() << "[MotorWorker] polling started, interval =" << intervalMs << "ms";
}

void MotorWorker::stopPolling()
{
    if (m_pollTimer && m_pollTimer->isActive()) {
        m_pollTimer->stop();
        qInfo() << "[MotorWorker] polling stopped";
    }
}

void MotorWorker::pollStatusOnce()
{
    if (!m_controller || !m_controller->isConnected()) {
        return;
    }

    /* 一次读取六轴整帧状态，避免界面标签在不同轮询周期内出现前后不一致。 */
    QVector<float> pos;
    QVector<float> actualPos;
    QVector<float> spd;
    QVector<qint32> runningStates;
    QVector<quint32> homeState;

    const bool ok1 = m_controller->getAxisPos(MotorController::kAllAxis, pos);
    const bool ok2 = m_controller->getAxisActualPos(MotorController::kAllAxis, actualPos);
    const bool ok3 = m_controller->getAxisSpd(MotorController::kAllAxis, spd);
    const bool ok4 = m_controller->getAxisRunningStates(runningStates);
    const bool ok5 = m_controller->getHomeState(homeState);

    if (ok1 && ok2 && ok3 && ok4 && ok5) {
        emit statusUpdated(pos, actualPos, spd, runningStates, homeState);
        return;
    }

    qWarning() << "[MotorWorker] pollStatusOnce failed";
}
