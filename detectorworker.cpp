#include "detectorworker.h"

#include <QDebug>
#include <QMetaObject>
#include <QMetaType>
#include <QtGlobal>

namespace
{
constexpr int kImagePollIntervalMs = 20;
constexpr quint64 kMinImageBufferBytes = 8ULL * 1024ULL * 1024ULL;
constexpr quint64 kBufferedFrameCount = 8ULL;
constexpr int kInvokeTimeoutMs = 2000;
}

DetectorWorker::DetectorWorker(QObject* parent)
    : QObject(parent)
    , m_controller(nullptr)
{
    qRegisterMetaType<QString>("QString");
    qRegisterMetaType<QByteArray>("QByteArray");
}

DetectorWorker::~DetectorWorker()
{
    shutdown();
}

void DetectorWorker::initialize(const QString& workDir)
{
    if (!m_controller) {
        m_controller = std::make_unique<DetectorController>();

        /*
         * 厂商 SDK 回调线程不可控，因此这里统一 repost 回 worker 自己的线程。
         * 后续所有状态变更都在同一线程内串行执行，避免锁和跨线程 UI 问题。
         */
        m_controller->setErrorCallback([this](const QString& msg) {
            QMetaObject::invokeMethod(
                this,
                "onControllerError",
                Qt::QueuedConnection,
                Q_ARG(QString, msg));
        });

        m_controller->setEventCallback([this](int detectorId,
                                              int eventId,
                                              int eventLevel,
                                              const QString& msg,
                                              int param1,
                                              int param2) {
            QMetaObject::invokeMethod(
                this,
                "onControllerEvent",
                Qt::QueuedConnection,
                Q_ARG(int, detectorId),
                Q_ARG(int, eventId),
                Q_ARG(int, eventLevel),
                Q_ARG(QString, msg),
                Q_ARG(int, param1),
                Q_ARG(int, param2));
        });

        m_controller->setImageCallback([this](const QByteArray& image,
                                              int imageSize,
                                              int frameNo,
                                              int width,
                                              int height,
                                              int bytesPerPixel) {
            QMetaObject::invokeMethod(
                this,
                "onControllerImage",
                Qt::QueuedConnection,
                Q_ARG(QByteArray, image),
                Q_ARG(int, imageSize),
                Q_ARG(int, frameNo),
                Q_ARG(int, width),
                Q_ARG(int, height),
                Q_ARG(int, bytesPerPixel));
        });
    }

    if (!m_imagePollTimer) {
        /* 图像缓存采用“短周期轮询 + 一次尽量排空”的策略，减少预览延迟。 */
        m_imagePollTimer = new QTimer(this);
        m_imagePollTimer->setInterval(kImagePollIntervalMs);
        m_imagePollTimer->setSingleShot(false);
        connect(m_imagePollTimer, &QTimer::timeout,
                this, &DetectorWorker::pollBufferedImages);
    }

    QString err;
    const bool ok = m_controller->initialize(workDir, err);
    if (!ok) {
        emitError(err);
    } else {
        m_currentCorrectOption = Enm_CorrectOp_Null;
    }

    emit initialized(ok);
}

void DetectorWorker::shutdown()
{
    stopPollingInternal(true);

    if (m_controller) {
        m_controller->shutdown();
        m_controller.reset();
    }
}

void DetectorWorker::scanOnce(const QString& ip)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->scanOnce(ip, err)) {
        emitError(err);
    }
}

void DetectorWorker::detectorIsConnected()
{
    const bool ok = m_controller && m_controller->detectorIsConnected();
    emit connected(ok);
}

void DetectorWorker::setAttrInt(int attrId, int value)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->setAttrInt(attrId, value, err)) {
        emitError(err);
    }
}

void DetectorWorker::setAttrFloat(int attrId, float value)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->setAttrFloat(attrId, value, err)) {
        emitError(err);
    }
}

void DetectorWorker::setAttrStr(int attrId, const QString& value)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->setAttrStr(attrId, value, err)) {
        emitError(err);
    }
}

void DetectorWorker::getAttrInt(int attrId)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    int value = 0;
    if (!m_controller->getAttrInt(attrId, value, err)) {
        emitError(err);
        return;
    }

    emit attrIntRead(attrId, value);
}

void DetectorWorker::getAttrFloat(int attrId)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    float value = 0.0f;
    if (!m_controller->getAttrFloat(attrId, value, err)) {
        emitError(err);
        return;
    }

    emit attrFloatRead(attrId, value);
}

void DetectorWorker::getAttrStr(int attrId)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    QString value;
    if (!m_controller->getAttrStr(attrId, value, err)) {
        emitError(err);
        return;
    }

    emit attrStrRead(attrId, value);
}

void DetectorWorker::invoke(int cmdId, const QVariantList& args)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->invoke(cmdId, args, err)) {
        emitError(err);
    }
}

void DetectorWorker::syncInvoke(int cmdId, const QVariantList& args, int timeoutMs)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->syncInvoke(cmdId, args, timeoutMs, err)) {
        emitError(err);
    }
}

void DetectorWorker::readCurrentCorrectOption()
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    int option = 0;
    if (!m_controller->getAttrInt(Attr_CurrentCorrectOption, option, err)) {
        emitError(err);
        return;
    }

    m_currentCorrectOption = option;
    emit currentCorrectOptionRead(option);
}

void DetectorWorker::applyCorrectionSelection(bool enableOffset, bool enableGain, bool enableDefect)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    int currentOption = m_currentCorrectOption;
    if (!m_controller->getAttrInt(Attr_CurrentCorrectOption, currentOption, err)) {
        emitError(err);
        return;
    }

    // Preserve unrelated correction bits while replacing the three UI-controlled flags.
    int mergedOption = currentOption & ~(CDetector::OFFSETMASK | CDetector::GAINMASK | CDetector::DEFECTMASK);
    if (enableOffset)
        mergedOption |= CDetector::OFFSETMASK;
    if (enableGain)
        mergedOption |= CDetector::GAINMASK;
    if (enableDefect)
        mergedOption |= CDetector::DEFECTMASK;

    if (!m_controller->syncInvoke(Cmd_SetCorrectOption, QVariantList{mergedOption}, kInvokeTimeoutMs, err)) {
        m_currentCorrectOption = currentOption;
        emit currentCorrectOptionRead(currentOption);
        emitError(err);
        return;
    }

    m_currentCorrectOption = mergedOption;
    emit currentCorrectOptionApplied(mergedOption);
}

void DetectorWorker::setCaliSubset(const QString& subset)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    // Application mode changes can point the detector at a different calibration subset.
    if (!m_controller->syncInvoke(Cmd_SetCaliSubset, QVariantList{subset}, kInvokeTimeoutMs, err)) {
        emitError(err);
        return;
    }

    readCurrentCorrectOption();
}

void DetectorWorker::useImageBuffer(quint64 bufSizeBytes)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->useImageBuffer(bufSizeBytes, err)) {
        emitError(err);
    }
}

void DetectorWorker::clearImageBuffer()
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    if (!m_controller->clearImageBuffer(err)) {
        emitError(err);
    }
}

void DetectorWorker::fetchOneImage()
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        return;
    }

    DetectorController::ImageBufferInfo info;
    if (!m_controller->queryImageBufferInfo(info, err)) {
        emitError(err);
        return;
    }

    if (info.frameCount <= 0) {
        return;
    }

    if (!m_controller->fetchOneImage(info, err)) {
        emitError(err);
    }
}

void DetectorWorker::startSingleAcquisition(int expectedWidth, int expectedHeight, int expectedBytesPerPixel)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        emit acquisitionStateChanged(false, false);
        return;
    }

    if (m_acquisitionActive) {
        emitError("Detector acquisition is already running.");
        emit acquisitionStateChanged(true, m_continuousAcquisition);
        return;
    }

    /* 为预览和落盘预留多帧缓存，避免单帧模式下刚触发就因缓存不足丢帧。 */
    if (!prepareImageBuffer(expectedWidth, expectedHeight, expectedBytesPerPixel, err)) {
        emitError(err);
        emit acquisitionStateChanged(false, false);
        return;
    }

    if (!m_controller->syncInvoke(Cmd_ForceSingleAcq, QVariantList{}, kInvokeTimeoutMs, err)) {
        emitError(err);
        emit acquisitionStateChanged(false, false);
        return;
    }

    m_acquisitionActive = true;
    m_continuousAcquisition = false;
    if (m_imagePollTimer)
        m_imagePollTimer->start();

    emit acquisitionStateChanged(true, false);
}

void DetectorWorker::startContinuousAcquisition(int expectedWidth, int expectedHeight, int expectedBytesPerPixel)
{
    QString err;
    if (!ensureController(err)) {
        emitError(err);
        emit acquisitionStateChanged(false, false);
        return;
    }

    if (m_acquisitionActive) {
        emitError("Detector acquisition is already running.");
        emit acquisitionStateChanged(true, m_continuousAcquisition);
        return;
    }

    /* 连续采集与单帧采集共用同一条缓存出图链路，仅底层启动命令不同。 */
    if (!prepareImageBuffer(expectedWidth, expectedHeight, expectedBytesPerPixel, err)) {
        emitError(err);
        emit acquisitionStateChanged(false, false);
        return;
    }

    if (!m_controller->syncInvoke(Cmd_StartAcq, QVariantList{}, kInvokeTimeoutMs, err)) {
        emitError(err);
        emit acquisitionStateChanged(false, false);
        return;
    }

    m_acquisitionActive = true;
    m_continuousAcquisition = true;
    if (m_imagePollTimer)
        m_imagePollTimer->start();

    emit acquisitionStateChanged(true, true);
}

void DetectorWorker::stopAcquisition()
{
    if (!m_controller) {
        stopPollingInternal(true);
        return;
    }

    QString err;
    if (m_continuousAcquisition &&
        !m_controller->syncInvoke(Cmd_StopAcq, QVariantList{}, kInvokeTimeoutMs, err)) {
        stopPollingInternal(true);
        emitError(err);
        return;
    }

    stopPollingInternal(true);
}

void DetectorWorker::onControllerError(const QString& msg)
{
    if (m_acquisitionActive)
        stopPollingInternal(true);

    emit errorOccurred(msg);
}

void DetectorWorker::onControllerEvent(int detectorId,
                                       int eventId,
                                       int eventLevel,
                                       const QString& msg,
                                       int param1,
                                       int param2)
{
    emit detectorEvent(detectorId, eventId, eventLevel, msg, param1, param2);
}

void DetectorWorker::onControllerImage(const QByteArray& image,
                                       int imageSize,
                                       int frameNo,
                                       int width,
                                       int height,
                                       int bytesPerPixel)
{
    emit imageReady(image, imageSize, frameNo, width, height, bytesPerPixel);

    if (m_acquisitionActive && !m_continuousAcquisition)
        stopPollingInternal(true);
}

void DetectorWorker::pollBufferedImages()
{
    if (!m_controller || !m_acquisitionActive)
        return;

    /* 每次超时尽量把当前缓存中的帧全部取完，降低 UI 预览滞后。 */
    while (m_acquisitionActive) {
        QString err;
        DetectorController::ImageBufferInfo info;
        if (!m_controller->queryImageBufferInfo(info, err)) {
            stopPollingInternal(true);
            emitError(err);
            return;
        }

        if (info.frameCount <= 0)
            return;

        if (!m_controller->fetchOneImage(info, err)) {
            stopPollingInternal(true);
            emitError(err);
            return;
        }
    }
}

bool DetectorWorker::ensureController(QString& errMsg)
{
    if (m_controller)
        return true;

    errMsg = "Detector is not initialized.";
    return false;
}

bool DetectorWorker::prepareImageBuffer(int expectedWidth,
                                        int expectedHeight,
                                        int expectedBytesPerPixel,
                                        QString& errMsg)
{
    /*
     * 缓冲区大小由预期帧尺寸推导，但仍保留一个固定下限。
     * 这样即便 ROI 很小，也能承受短时间内突发的 SDK 回调。
     */
    const quint64 safeWidth = static_cast<quint64>(qMax(expectedWidth, 1));
    const quint64 safeHeight = static_cast<quint64>(qMax(expectedHeight, 1));
    const quint64 safeBytesPerPixel = static_cast<quint64>(qMax(expectedBytesPerPixel, 2));
    const quint64 frameBytes = safeWidth * safeHeight * safeBytesPerPixel;
    const quint64 bufferBytes = qMax(frameBytes * kBufferedFrameCount, kMinImageBufferBytes);

    if (!m_controller->useImageBuffer(bufferBytes, errMsg))
        return false;

    if (!m_controller->clearImageBuffer(errMsg))
        return false;

    return true;
}

void DetectorWorker::stopPollingInternal(bool emitStateChanged)
{
    if (m_imagePollTimer)
        m_imagePollTimer->stop();

    const bool wasActive = m_acquisitionActive || m_continuousAcquisition;
    m_acquisitionActive = false;
    m_continuousAcquisition = false;

    if (emitStateChanged && wasActive)
        emit acquisitionStateChanged(false, false);
}

void DetectorWorker::emitError(const QString& msg)
{
    qWarning() << "[DetectorWorker]" << msg;
    emit errorOccurred(msg);
}
