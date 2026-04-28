#ifndef MOTORWORKER_H
#define MOTORWORKER_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <memory>

class MotorController;

/*
 * MotorWorker 是主窗口和 MotorController 之间的异步边界。
 * 所有运动命令、轮询和错误处理都收敛到这个线程对象里，
 * UI 只通过 queued connection 发送命令，避免直接阻塞串口/DLL 调用。
 */
class MotorWorker : public QObject
{
    Q_OBJECT

public:
    explicit MotorWorker(QObject* parent = nullptr);
    ~MotorWorker() override;

public slots:
    void initialize();
    void shutdown();

    void connectSerial(quint8 comPort);
    void connectNetwork(const QString& ip, qint32 port);
    void disconnectController();

    void absMove(quint8 axisId, float pos, float vel, float acc);
    void relMove(quint8 axisId, float dist, float vel, float acc);
    void moveAtSpeed(quint8 axisId, float vel, float acc);

    void stopAxis(quint8 axisId, float dec);
    void emergencyStopAxis(quint8 axisId);
    void pauseAxis(quint8 axisId);
    void restartAxis(quint8 axisId);

    void seekZero(quint8 axisId, float vel, float acc);
    void cancelSeekZero(quint8 axisId);

    void startPolling(int intervalMs = 200);
    void stopPolling();
    /* 一次读取六轴快照，并作为同一帧状态统一推给 UI。 */
    void pollStatusOnce();

signals:
    void connectedChanged(bool connected);
    void statusUpdated(QVector<float> pos,
                       QVector<float> actualPos,
                       QVector<float> spd,
                       QVector<qint32> runningStates,
                       QVector<quint32> homeState);
    void operationFinished(const QString& opName, bool ok, const QString& message);
    void errorOccurred(const QString& title, const QString& message);

private:
    void emitUiError(const QString& title, const QString& message);
    /* 把底层失败统一折叠成 UI 可以直接展示的错误路径。 */
    void notifyIfFailed(const QString& opName, bool ok, const QString& fallbackMessage);

private:
    std::unique_ptr<MotorController> m_controller;
    QTimer* m_pollTimer = nullptr;
};

#endif // MOTORWORKER_H
