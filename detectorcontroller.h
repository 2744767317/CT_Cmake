#pragma once

#include <functional>
#include <memory>
#include <string>

#include <QString>
#include <QByteArray>
#include <QVariantList>

#include "detector.h"

/*
 * DetectorController 是探测器 SDK 的薄封装层。
 * 它不管理线程，只负责：
 * 1. 打开/关闭厂商库与 CDetector 实例；
 * 2. 把 Qt/应用层参数转换成 SDK 需要的类型；
 * 3. 暴露图像缓存查询、命令调用、属性读写与 SDK 回调入口。
 *
 * 线程模型上，这个类始终由 DetectorWorker 在工作线程内独占使用，
 * MainWindow 不会直接跨线程调用它。
 */
class DetectorController : public iSDKCallback
{
public:
    /* 图像缓存当前可读状态的快照。 */
    struct ImageBufferInfo
    {
        int frameCount = 0;
        int width = 0;
        int height = 0;
        int bytesPerPixel = 0;
        int imageSize = 0;
        int propListMemSize = 0;
    };

    using ErrorCallback = std::function<void(const QString&)>;
    using EventCallback = std::function<void(int detectorId,
                                             int eventId,
                                             int eventLevel,
                                             const QString& msg,
                                             int param1,
                                             int param2)>;
    using ImageCallback = std::function<void(const QByteArray& image,
                                             int imageSize,
                                             int frameNo,
                                             int width,
                                             int height,
                                             int bytesPerPixel)>;

    DetectorController();
    ~DetectorController();

    /* 初始化 SDK 并在工作目录下创建探测器对象。 */
    bool initialize(const QString& workDir, QString& errMsg);
    /* 释放探测器对象和动态库句柄。 */
    void shutdown();

    bool scanOnce(const QString& ip, QString& errMsg);
    bool detectorIsConnected();

    bool setAttrInt(int attrId, int value, QString& errMsg);
    bool setAttrFloat(int attrId, float value, QString& errMsg);
    bool setAttrStr(int attrId, const QString& value, QString& errMsg);

    bool getAttrInt(int attrId, int& value, QString& errMsg);
    bool getAttrFloat(int attrId, float& value, QString& errMsg);
    bool getAttrStr(int attrId, QString& value, QString& errMsg);

    bool invoke(int cmdId, const QVariantList& args, QString& errMsg);
    bool syncInvoke(int cmdId, const QVariantList& args, int timeoutMs, QString& errMsg);

    bool useImageBuffer(quint64 bufSizeBytes, QString& errMsg);
    bool clearImageBuffer(QString& errMsg);
    bool queryImageBufferInfo(ImageBufferInfo& info, QString& errMsg);
    bool fetchOneImage(const ImageBufferInfo& info, QString& errMsg);

    void setErrorCallback(ErrorCallback cb);
    void setEventCallback(EventCallback cb);
    void setImageCallback(ImageCallback cb);

    bool isInitialized() const;
    bool isConnected() const;

    /* SDK 原始回调统一先落到这里，再转发给上层回调。 */
    void UserCallbackHandler(int nDetectorID,
                             int nEventID,
                             int nEventLevel,
                             const char* pszMsg,
                             int nParam1,
                             int nParam2,
                             int nPtrParamLen,
                             void* pParam) override;

private:
    /* 把 SDK 返回码包装成统一的日志/错误信息。 */
    QString fpdErrorToString(int code) const;
    bool checkResult(int ret, QString& errMsg, const QString& action) const;

private:
    std::unique_ptr<SDKAPIHelper> m_sdk;
    std::unique_ptr<CDetector> m_detector;

    bool m_initialized = false;
    bool m_connected = false;

    ErrorCallback m_errorCallback;
    EventCallback m_eventCallback;
    ImageCallback m_imageCallback;
};
