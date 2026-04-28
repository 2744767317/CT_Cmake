#include "MotorController.h"

#include <QByteArray>
#include <QDebug>
#include <QMutexLocker>

namespace {
constexpr quint16 kFunResOk = 0x01;
}

MotorController::MotorController(QObject* parent)
    : QObject(parent)
{
    ensureCard();
    qInfo() << "[MotorController] created";
}

MotorController::~MotorController()
{
    if (m_connected) {
        qInfo() << "[MotorController] disconnect in destructor";
        disconnectController();
    }
    qInfo() << "[MotorController] destroyed";
}

bool MotorController::ensureCard()
{
    if (!m_card) {
        /* DLL 封装对象内部带状态，因此整个控制器生命周期内只保留一个实例。 */
        m_card = std::make_unique<MoCtrCard>();
        qInfo() << "[MotorController] MoCtrCard instance created";
    }
    return true;
}

void MotorController::setLastError(const QString& err)
{
    m_lastError = err;
}

QString MotorController::lastError() const
{
    QMutexLocker locker(&m_mutex);
    return m_lastError;
}

MotorController::ConnectType MotorController::connectType() const
{
    QMutexLocker locker(&m_mutex);
    return m_connectType;
}

bool MotorController::connectSerial(quint8 comPort)
{
    /* 厂商 DLL 内部会缓存连接态，因此重连前先彻底断开，避免残留状态污染。 */
    if (isConnected()) {
        qInfo() << "[MotorController] already connected, disconnect first";
        if (!disconnectController()) {
            return false;
        }
    }

    QMutexLocker locker(&m_mutex);
    ensureCard();
    m_lastError.clear();

    qInfo() << "[MotorController] connectSerial, COM =" << comPort;

    const quint16 ret = m_card->MoCtrCard_Initial(static_cast<McCard_UINT8>(comPort));
    if (!handleResult("MoCtrCard_Initial", ret, true)) {
        m_connected = false;
        m_connectType = ConnectType::None;
        return false;
    }

    m_connectType = ConnectType::Serial;
    return refreshConnectionState();
}

bool MotorController::connectNetwork(const QString& ip, qint32 port)
{
    /* 网络链路先保留在底层，方便后续界面切换到以太网控制。 */
    if (isConnected()) {
        qInfo() << "[MotorController] already connected, disconnect first";
        if (!disconnectController()) {
            return false;
        }
    }

    QMutexLocker locker(&m_mutex);
    ensureCard();
    m_lastError.clear();

    QByteArray ipBytes = ip.toLocal8Bit();
    qInfo() << "[MotorController] connectNetwork, ip =" << ip << ", port =" << port;

    const quint16 ret = m_card->MoCtrCard_Net_Initial(ipBytes.data(), static_cast<McCard_INT32>(port));
    if (!handleResult("MoCtrCard_Net_Initial", ret, true)) {
        m_connected = false;
        m_connectType = ConnectType::None;
        return false;
    }

    m_connectType = ConnectType::Network;
    return refreshConnectionState();
}

bool MotorController::disconnectController()
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!m_card) {
        qWarning() << "[MotorController] disconnectController: m_card is null";
        m_connected = false;
        m_connectType = ConnectType::None;
        return true;
    }

    qInfo() << "[MotorController] disconnectController";

    const quint16 ret = m_card->MoCtrCard_Unload();
    if (!handleResult("MoCtrCard_Unload", ret, true)) {
        return false;
    }

    m_connected = false;
    m_connectType = ConnectType::None;
    return true;
}

bool MotorController::isConnected() const
{
    QMutexLocker locker(&m_mutex);
    return m_connected;
}

bool MotorController::refreshConnectionState()
{
    if (!m_card) {
        m_connected = false;
        m_connectType = ConnectType::None;
        setLastError("Controller object is null.");
        qCritical() << "[MotorController] refreshConnectionState failed: m_card is null";
        return false;
    }

    McCard_INT32 comm[1] = {0};
    McCard_UINT8 link[1] = {0};

    const quint16 ret1 = m_card->MoCtrCard_GetCommState(comm);
    const quint16 ret2 = m_card->MoCtrCard_GetLinkState(link);

    if (ret1 != kFunResOk || ret2 != kFunResOk) {
        m_connected = false;
        setLastError("Failed to query communication/link state.");
        qCritical() << "[MotorController] refreshConnectionState failed,"
                    << "ret1 =" << Qt::hex << ret1
                    << "ret2 =" << ret2 << Qt::dec;
        return false;
    }

    /* 通信状态和链路状态都正常时，才认为控制器真正可用。 */
    m_connected = (comm[0] != 0) && (link[0] != 0);

    qInfo() << "[MotorController] refreshConnectionState:"
            << "comm =" << comm[0]
            << "link =" << static_cast<int>(link[0])
            << "connected =" << m_connected;

    if (!m_connected) {
        setLastError("Controller initialized but link is not established.");
    }

    return m_connected;
}

bool MotorController::getCommState(int& state)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!m_card) {
        setLastError("Controller object is null.");
        qCritical() << "[MotorController] getCommState failed: m_card is null";
        return false;
    }

    McCard_INT32 out[1] = {0};
    const quint16 ret = m_card->MoCtrCard_GetCommState(out);
    if (!handleResult("MoCtrCard_GetCommState", ret, false)) {
        return false;
    }

    state = static_cast<int>(out[0]);
    qDebug() << "[MotorController] commState =" << state;
    return true;
}

bool MotorController::getLinkState(bool& linked)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!m_card) {
        setLastError("Controller object is null.");
        qCritical() << "[MotorController] getLinkState failed: m_card is null";
        return false;
    }

    McCard_UINT8 out[1] = {0};
    const quint16 ret = m_card->MoCtrCard_GetLinkState(out);
    if (!handleResult("MoCtrCard_GetLinkState", ret, false)) {
        return false;
    }

    linked = (out[0] != 0);
    qDebug() << "[MotorController] linkState =" << linked;
    return true;
}

bool MotorController::getAxisPos(quint8 axisId, QVector<float>& pos)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId, true)) {
        return false;
    }

    float out[kMaxAxis] = {0.0f};
    const quint16 ret = m_card->MoCtrCard_GetAxisPos(axisId, out);
    if (!handleResult("MoCtrCard_GetAxisPos", ret, false)) {
        return false;
    }

    /* DLL 固定写入数组，这里压成 QVector，方便上层统一按索引访问。 */
    pos.clear();
    if (axisId == kAllAxis) {
        for (int i = 0; i < kMaxAxis; ++i) pos.push_back(out[i]);
    } else {
        pos.push_back(out[0]);
    }
    return true;
}

bool MotorController::getAxisActualPos(quint8 axisId, QVector<float>& pos)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId, true)) {
        return false;
    }

    float out[kMaxAxis] = {0.0f};
    const quint16 ret = m_card->MoCtrCard_GetAxisActualPos(axisId, out);
    if (!handleResult("MoCtrCard_GetAxisActualPos", ret, false)) {
        return false;
    }

    /* 单轴读 1 个值，全轴读 6 个值，和指令位置接口保持同一打包规则。 */
    pos.clear();
    if (axisId == kAllAxis) {
        for (int i = 0; i < kMaxAxis; ++i) pos.push_back(out[i]);
    } else {
        pos.push_back(out[0]);
    }
    return true;
}

bool MotorController::getAxisSpd(quint8 axisId, QVector<float>& spd)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId, true)) {
        return false;
    }

    float out[kMaxAxis] = {0.0f};
    const quint16 ret = m_card->MoCtrCard_GetAxisSpd(axisId, out);
    if (!handleResult("MoCtrCard_GetAxisSpd", ret, false)) {
        return false;
    }

    /* 速度接口沿用与位置接口相同的数组收口规则。 */
    spd.clear();
    if (axisId == kAllAxis) {
        for (int i = 0; i < kMaxAxis; ++i) spd.push_back(out[i]);
    } else {
        spd.push_back(out[0]);
    }
    return true;
}

bool MotorController::getRunState(QVector<qint32>& runState)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__)) {
        return false;
    }

    McCard_INT32 out[kMaxAxis] = {0};
    const quint16 ret = m_card->MoCtrCard_GetRunState(out);
    if (!handleResult("MoCtrCard_GetRunState", ret, false)) {
        return false;
    }

    runState.clear();
    for (int i = 0; i < kMaxAxis; ++i) runState.push_back(out[i]);
    return true;
}

bool MotorController::getHomeState(QVector<quint32>& homeState)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__)) {
        return false;
    }

    McCard_UINT32 out[kMaxAxis] = {0};
    const quint16 ret = m_card->MoCtrCard_GetHomeState(out);
    if (!handleResult("MoCtrCard_GetHomeState", ret, false)) {
        return false;
    }

    homeState.clear();
    for (int i = 0; i < kMaxAxis; ++i) {
        homeState.push_back(static_cast<quint32>(out[i]));
    }
    return true;
}

bool MotorController::getAxisRunningStates(QVector<qint32>& runningStates)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__)) {
        return false;
    }

    /* 运行状态只能逐轴查询，因此在控制器层先聚合成一份六轴快照。 */
    runningStates.clear();
    for (quint8 axisId = 0; axisId < kMaxAxis; ++axisId) {
        McCard_INT32 out[1] = {0};
        const quint16 ret = m_card->MoCtrCard_IsAxisRunning(axisId, out);
        if (!handleResult(QString("MoCtrCard_IsAxisRunning(axis=%1)").arg(axisId), ret, false)) {
            runningStates.clear();
            return false;
        }

        runningStates.push_back(static_cast<qint32>(out[0]));
    }

    return true;
}

bool MotorController::absMove(quint8 axisId, float pos, float vel, float acc)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    /* DLL 轴号是 0 基，对应界面 X/Y/Z/A/B/C 六轴。 */
    qInfo() << "[MotorController] absMove:"
            << "axis =" << axisId
            << "pos =" << pos
            << "vel =" << vel
            << "acc =" << acc;

    return handleResult("MoCtrCard_MCrlAxisAbsMove",
                        m_card->MoCtrCard_MCrlAxisAbsMove(axisId, pos, vel, acc),
                        true);
}

bool MotorController::relMove(quint8 axisId, float dist, float vel, float acc)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qInfo() << "[MotorController] relMove:"
            << "axis =" << axisId
            << "dist =" << dist
            << "vel =" << vel
            << "acc =" << acc;

    return handleResult("MoCtrCard_MCrlAxisRelMove",
                        m_card->MoCtrCard_MCrlAxisRelMove(axisId, dist, vel, acc),
                        true);
}

bool MotorController::moveAtSpeed(quint8 axisId, float vel, float acc)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qInfo() << "[MotorController] moveAtSpeed:"
            << "axis =" << axisId
            << "vel =" << vel
            << "acc =" << acc;

    return handleResult("MoCtrCard_MCrlAxisMoveAtSpd",
                        m_card->MoCtrCard_MCrlAxisMoveAtSpd(axisId, vel, acc),
                        true);
}

bool MotorController::stopAxis(quint8 axisId, float dec)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qInfo() << "[MotorController] stopAxis:"
            << "axis =" << axisId
            << "dec =" << dec;

    return handleResult("MoCtrCard_StopAxisMov",
                        m_card->MoCtrCard_StopAxisMov(axisId, dec),
                        true);
}

bool MotorController::emergencyStopAxis(quint8 axisId)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qCritical() << "[MotorController] emergencyStopAxis, axis =" << axisId;

    return handleResult("MoCtrCard_EmergencyStopAxisMov",
                        m_card->MoCtrCard_EmergencyStopAxisMov(axisId),
                        true);
}

bool MotorController::pauseAxis(quint8 axisId)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qInfo() << "[MotorController] pauseAxis, axis =" << axisId;

    return handleResult("MoCtrCard_PauseAxisMov",
                        m_card->MoCtrCard_PauseAxisMov(axisId),
                        true);
}

bool MotorController::restartAxis(quint8 axisId)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qInfo() << "[MotorController] restartAxis, axis =" << axisId;

    return handleResult("MoCtrCard_ReStartAxisMov",
                        m_card->MoCtrCard_ReStartAxisMov(axisId),
                        true);
}

bool MotorController::seekZero(quint8 axisId, float vel, float acc)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    /* 回零必须走控制器自身流程，不能只在 UI 上把位置强行改成 0。 */
    qInfo() << "[MotorController] seekZero:"
            << "axis =" << axisId
            << "vel =" << vel
            << "acc =" << acc;

    return handleResult("MoCtrCard_SeekZero",
                        m_card->MoCtrCard_SeekZero(axisId, vel, acc),
                        true);
}

bool MotorController::cancelSeekZero(quint8 axisId)
{
    QMutexLocker locker(&m_mutex);
    m_lastError.clear();

    if (!ensureConnected(__FUNCTION__) || !validateAxis(axisId)) {
        return false;
    }

    qInfo() << "[MotorController] cancelSeekZero, axis =" << axisId;

    return handleResult("MoCtrCard_CancelSeekZero",
                        m_card->MoCtrCard_CancelSeekZero(axisId),
                        true);
}

bool MotorController::ensureConnected(const char* caller)
{
    /* 统一连接前置检查，保证每个公开接口的失败文案一致。 */
    if (!m_card) {
        const QString msg = QString("%1 failed: controller object is null.").arg(caller);
        setLastError(msg);
        qCritical() << "[MotorController]" << msg;
        return false;
    }

    if (!m_connected) {
        const QString msg = QString("%1 failed: controller is not connected.").arg(caller);
        setLastError(msg);
        qCritical() << "[MotorController]" << msg;
        return false;
    }

    return true;
}

bool MotorController::validateAxis(quint8 axisId, bool allowAllAxis)
{
    /* MCC6 轴号是 0 基；0xFF 只允许用于状态读取，不允许用于运动命令。 */
    if (allowAllAxis && axisId == kAllAxis) {
        return true;
    }
    if (axisId < kMaxAxis) {
        return true;
    }

    const QString msg = allowAllAxis
                            ? QString("Invalid axis id %1, valid range: 0~5 or 0xFF.").arg(axisId)
                            : QString("Invalid axis id %1, valid range: 0~5.").arg(axisId);

    setLastError(msg);
    qCritical() << "[MotorController]" << msg;
    return false;
}

bool MotorController::handleResult(const QString& apiName, quint16 ret, bool verboseSuccess)
{
    if (ret == kFunResOk) {
        if (verboseSuccess) {
            qInfo() << "[MotorController]" << apiName << "success";
        } else {
            qDebug() << "[MotorController]" << apiName << "success";
        }
        return true;
    }

    const QString err = QString("%1 failed, ret=0x%2 (%3)")
                            .arg(apiName)
                            .arg(ret, 0, 16)
                            .arg(errorString(ret));

    setLastError(err);
    qCritical() << "[MotorController]" << err;
    return false;
}

QString MotorController::errorString(quint16 code) const
{
    switch (code) {
    case 0x01: return "Success";
    case 0x02: return "Invalid axis id";
    case 0x03: return "Invalid output group/index";
    case 0x04: return "Invalid input group/index";
    case 0x05: return "Invalid interrupt input group/index";
    case 0x06: return "Open log info failed";
    case 0x07: return "G-code content error";
    case 0x08: return "G-code decode error";
    case 0x80: return "Open port failed";
    case 0x81: return "Callback success";
    case 0x82: return "Callback failed";
    case 0x83: return "Generic function error";
    default:
        return QString("Unknown error code: 0x%1").arg(code, 0, 16);
    }
}
