#ifndef MOTORCONTROLLER_H
#define MOTORCONTROLLER_H

#include <QObject>
#include <QMutex>
#include <QVector>
#include <QString>
#include <memory>

#include "motor/MCC6DLL.h"

/*
 * MotorController 是 MCC6 运动控制 DLL 的薄封装。
 * 它负责把 DLL 的“卡对象 + 返回码 + 固定长度数组”接口转换成 Qt 友好的形式。
 * 该类本身不做线程切换，所有公开接口都假定由 MotorWorker 在单一工作线程中顺序调用。
 */
class MotorController : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxAxis = 6;
    /* MCC6 约定 0xFF 表示一次读取全部六轴状态。 */
    static constexpr quint8 kAllAxis = 0xFF;

    enum class ConnectType {
        None,
        Serial,
        Network
    };
    Q_ENUM(ConnectType)

    explicit MotorController(QObject* parent = nullptr);
    ~MotorController() override;

    /* 当前界面主要使用串口，但底层也保留网络连接能力。 */
    bool connectSerial(quint8 comPort);
    bool connectNetwork(const QString& ip, qint32 port);
    bool disconnectController();

    bool isConnected() const;
    ConnectType connectType() const;

    bool refreshConnectionState();
    bool getCommState(int& state);
    bool getLinkState(bool& linked);

    bool getAxisPos(quint8 axisId, QVector<float>& pos);
    bool getAxisActualPos(quint8 axisId, QVector<float>& pos);
    bool getAxisSpd(quint8 axisId, QVector<float>& spd);
    bool getRunState(QVector<qint32>& runState);
    bool getHomeState(QVector<quint32>& homeState);
    bool getAxisRunningStates(QVector<qint32>& runningStates);

    bool absMove(quint8 axisId, float pos, float vel, float acc = 0.0f);
    bool relMove(quint8 axisId, float dist, float vel, float acc = 0.0f);
    bool moveAtSpeed(quint8 axisId, float vel, float acc = 0.0f);

    bool stopAxis(quint8 axisId, float dec);
    bool emergencyStopAxis(quint8 axisId);
    bool pauseAxis(quint8 axisId);
    bool restartAxis(quint8 axisId);

    bool seekZero(quint8 axisId, float vel, float acc = 0.0f);
    bool cancelSeekZero(quint8 axisId);

    QString lastError() const;
    QString errorString(quint16 code) const;

private:
    /* 延迟创建厂商卡对象，避免 MainWindow 启动时就做硬件初始化。 */
    bool ensureCard();
    bool ensureConnected(const char* caller);
    /* DLL 中轴号是 0 基，对应界面的 X/Y/Z/A/B/C。 */
    bool validateAxis(quint8 axisId, bool allowAllAxis = false);
    bool handleResult(const QString& apiName, quint16 ret, bool verboseSuccess = true);
    void setLastError(const QString& err);

private:
    mutable QMutex m_mutex;
    std::unique_ptr<MoCtrCard> m_card;
    ConnectType m_connectType = ConnectType::None;
    bool m_connected = false;
    QString m_lastError;
};

#endif // MOTORCONTROLLER_H
