#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

#include "xraycontroller.h"

/*
 * XRayWorker 把 X 射线源进程管理、TCP 通信和状态轮询都固定在工作线程里。
 * MainWindow 只通过信号告诉它“连接、开高压、关高压、改参数”，
 * 然后接收它回传的连接状态、高压状态和完整状态帧。
 */
class XRayWorker : public QObject
{
    Q_OBJECT

public:
    explicit XRayWorker(QObject* parent = nullptr);
    ~XRayWorker();

public slots:
    void initialize(const QString& vendorExePath,
                    const QStringList& vendorArgs = {},
                    const QString& workingDirectory = QString(),
                    const QString& host = "127.0.0.1",
                    const quint16& port = 3600);

    void shutdown();

    void openXRay();
    void closeXRay();
    void applySettings(double kv, int ua);
    void queryStatus();

signals:
    void errorOccurred(const QString& message);

    void initialized(bool ok);
    void connectionChanged(bool connected);
    void xrayStateChanged(bool hvOn);
    void statusUpdated(const XRayController::Status& status);

private:
    /* 先尝试直连端口；失败后再拉起厂商软件并重试连接。 */
    bool connectWithAutoStart();
    void startPolling();
    void stopPolling();
    /* 把控制器缓存的最新状态重新发回 UI。 */
    void publishCurrentStatus();
    void emitControllerError();
    void cleanupController(bool stopVendorSoftware);
    /* 轮询失败时，把链路故障统一转成“断开连接”的 UI 状态。 */
    void pollStatus();

private:
    std::unique_ptr<XRayController> m_controller;
    QTimer* m_pollTimer = nullptr;
    QString m_vendorExePath;
    QStringList m_vendorArgs;
    QString m_workingDirectory;
    QString m_host = QStringLiteral("127.0.0.1");
    quint16 m_port = 3600;
};
