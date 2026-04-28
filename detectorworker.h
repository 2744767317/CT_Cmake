#pragma once

#include <QObject>
#include <QTimer>
#include <memory>
#include <QVariantList>

#include "detectorcontroller.h"

/*
 * DetectorWorker 运行在独立线程中，独占持有 DetectorController。
 * 它负责把 MainWindow 发来的“连接、设置参数、开始采集”等请求串行化，
 * 同时把 SDK 回调和缓存轮询结果重新发布成 Qt 信号。
 */
class DetectorWorker : public QObject
{
    Q_OBJECT
public:
    explicit DetectorWorker(QObject* parent = nullptr);
    ~DetectorWorker();

public slots:
    void initialize(const QString& workDir);
    void shutdown();

    void scanOnce(const QString& ip);
    void detectorIsConnected();

    void setAttrInt(int attrId, int value);
    void setAttrFloat(int attrId, float value);
    void setAttrStr(int attrId, const QString& value);

    void getAttrInt(int attrId);
    void getAttrFloat(int attrId);
    void getAttrStr(int attrId);

    void invoke(int cmdId, const QVariantList& args);
    void syncInvoke(int cmdId, const QVariantList& args, int timeoutMs);

    void readCurrentCorrectOption();
    /* 把 UI 三个校正开关并回探测器当前校正位掩码。 */
    void applyCorrectionSelection(bool enableOffset, bool enableGain, bool enableDefect);
    /* 根据当前应用模式切换对应的校正子集。 */
    void setCaliSubset(const QString& subset);

    void useImageBuffer(quint64 bufSizeBytes);
    void clearImageBuffer();
    void fetchOneImage();

    /* 启动采集，并通过定时轮询把 SDK 图像缓存持续排空。 */
    void startSingleAcquisition(int expectedWidth, int expectedHeight, int expectedBytesPerPixel = 2);
    void startContinuousAcquisition(int expectedWidth, int expectedHeight, int expectedBytesPerPixel = 2);
    void stopAcquisition();

signals:
    void initialized(bool ok);
    void connected(bool ok);

    void attrIntRead(int attrId, int value);
    void attrFloatRead(int attrId, float value);
    void attrStrRead(int attrId, const QString& value);

    void detectorEvent(int detectorId,
                       int eventId,
                       int eventLevel,
                       const QString& msg,
                       int param1,
                       int param2);

    void currentCorrectOptionRead(int option);
    void currentCorrectOptionApplied(int option);

    void acquisitionStateChanged(bool running, bool continuous);

    void imageReady(const QByteArray& image,
                    int imageSize,
                    int frameNo,
                    int width,
                    int height,
                    int bytesPerPixel);

    void errorOccurred(const QString& message);

private slots:
    void onControllerError(const QString& msg);
    void onControllerEvent(int detectorId,
                           int eventId,
                           int eventLevel,
                           const QString& msg,
                           int param1,
                           int param2);
    void onControllerImage(const QByteArray& image,
                           int imageSize,
                           int frameNo,
                           int width,
                           int height,
                           int bytesPerPixel);
    void pollBufferedImages();

private:
    bool ensureController(QString& errMsg);
    /* 根据预期图像尺寸配置 SDK 缓存，并保留一个保守的最小缓存容量。 */
    bool prepareImageBuffer(int expectedWidth, int expectedHeight, int expectedBytesPerPixel, QString& errMsg);
    void stopPollingInternal(bool emitStateChanged);
    void emitError(const QString& msg);

private:
    std::unique_ptr<DetectorController> m_controller;
    QTimer* m_imagePollTimer = nullptr;
    bool m_acquisitionActive = false;
    bool m_continuousAcquisition = false;
    int m_currentCorrectOption = Enm_CorrectOp_Null;
};
