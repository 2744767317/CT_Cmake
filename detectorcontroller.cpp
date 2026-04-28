#include "detectorcontroller.h"

#include <QDir>
#include <QDebug>
#include <QMetaType>

DetectorController::DetectorController()
{
    qRegisterMetaType<QByteArray>("QByteArray");
}

DetectorController::~DetectorController()
{
    shutdown();
}

bool DetectorController::initialize(const QString& workDir, QString& errMsg)
{
    if (m_initialized) {
        qInfo() << "[DetectorController] already initialized";
        return true;
    }

    /* 初始化顺序固定为：打开 SDK 动态库 -> 创建 CDetector -> 绑定回调。 */
    qInfo() << "[DetectorController] initialize, workDir =" << workDir;

    m_sdk = std::make_unique<SDKAPIHelper>();
    int ret = m_sdk->OpenLibrary();
    if (!checkResult(ret, errMsg, "OpenLibrary")) {
        return false;
    }

    m_detector = std::make_unique<CDetector>(m_sdk.get());

    QByteArray workDirBytes = QDir::toNativeSeparators(workDir).toLocal8Bit();
    ret = m_detector->Create(workDirBytes.constData(), this);
    if (!checkResult(ret, errMsg, "Create detector")) {
        m_detector.reset();
        m_sdk->CloseLibrary();
        m_sdk.reset();
        return false;
    }

    m_initialized = true;
    m_connected = true;
    qInfo() << "[DetectorController] detector created successfully, detectorId ="
            << m_detector->DetectorID();
    return true;
}

void DetectorController::shutdown()
{
    if (!m_initialized) {
        return;
    }

    qInfo() << "[DetectorController] shutdown";

    if (m_detector) {
        m_detector->Destroy();
        m_detector.reset();
    }

    if (m_sdk) {
        m_sdk->CloseLibrary();
        m_sdk.reset();
    }

    m_initialized = false;
    m_connected = false;
}

bool DetectorController::scanOnce(const QString& ip, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    QByteArray ipBytes = ip.toLocal8Bit();
    int ret = m_detector->ScanOnce(ipBytes.constData());
    return checkResult(ret, errMsg, "ScanOnce");
}

bool DetectorController::detectorIsConnected()
{
    return m_initialized and m_connected;
}

bool DetectorController::setAttrInt(int attrId, int value, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    int ret = m_detector->SetAttr(attrId, value);
    return checkResult(ret, errMsg, QString("SetAttr(int), attr=%1").arg(attrId));
}

bool DetectorController::setAttrFloat(int attrId, float value, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    int ret = m_detector->SetAttr(attrId, value);
    return checkResult(ret, errMsg, QString("SetAttr(float), attr=%1").arg(attrId));
}

bool DetectorController::setAttrStr(int attrId, const QString& value, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    QByteArray bytes = value.toLocal8Bit();
    int ret = m_detector->SetAttr(attrId, bytes.constData());
    return checkResult(ret, errMsg, QString("SetAttr(string), attr=%1").arg(attrId));
}

bool DetectorController::getAttrInt(int attrId, int& value, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    int ret = m_detector->GetAttr(attrId, value);
    return checkResult(ret, errMsg, QString("GetAttr(int), attr=%1").arg(attrId));
}

bool DetectorController::getAttrFloat(int attrId, float& value, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    int ret = m_detector->GetAttr(attrId, value);
    return checkResult(ret, errMsg, QString("GetAttr(float), attr=%1").arg(attrId));
}

bool DetectorController::getAttrStr(int attrId, QString& value, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    std::string tmp;
    int ret = m_detector->GetAttr(attrId, tmp);
    if (!checkResult(ret, errMsg, QString("GetAttr(string), attr=%1").arg(attrId))) {
        return false;
    }

    value = QString::fromStdString(tmp);
    return true;
}

bool DetectorController::invoke(int cmdId, const QVariantList& args, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    /* 应用层用 QVariantList 表示命令参数，这里统一降成 SDK 的 IRayCmdObject。 */
    std::vector<IRayCmdObject> cmdArgs;
    cmdArgs.reserve(args.size());

    for (const QVariant& v : args) {
        switch (v.typeId()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Bool:
            cmdArgs.emplace_back(v.toInt());
            break;

        case QMetaType::Float:
        case QMetaType::Double:
            cmdArgs.emplace_back(v.toDouble());
            break;

        case QMetaType::QString:
        case QMetaType::QByteArray:
            cmdArgs.emplace_back(v.toString().toStdString());
            break;

        default:
            errMsg = QString("Invoke, cmd=%1 failed: unsupported QVariant type (%2)")
                         .arg(cmdId)
                         .arg(QString::fromLatin1(v.typeName() ? v.typeName() : "unknown"));
            return false;
        }
    }

    int ret = Err_InvalidParamValue;

    switch (cmdArgs.size()) {
    case 0: ret = m_detector->Invoke(cmdId); break;
    case 1: ret = m_detector->Invoke(cmdId, cmdArgs[0]); break;
    case 2: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1]); break;
    case 3: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2]); break;
    case 4: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3]); break;
    case 5: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4]); break;
    case 6: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], cmdArgs[5]); break;
    case 7: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], cmdArgs[5], cmdArgs[6]); break;
    case 8: ret = m_detector->Invoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], cmdArgs[5], cmdArgs[6], cmdArgs[7]); break;
    default:
        errMsg = QString("Invoke, cmd=%1 failed: too many parameters (%2), max supported = 8")
                     .arg(cmdId)
                     .arg(cmdArgs.size());
        return false;
    }

    if (ret == Err_OK || ret == Err_TaskPending) {
        qInfo() << "[DetectorController] Invoke accepted, cmd =" << cmdId
                << ", ret =" << ret
                << ", argCount =" << args.size();
        return true;
    }

    errMsg = QString("Invoke, cmd=%1 failed: %2").arg(cmdId).arg(fpdErrorToString(ret));
    qWarning() << "[DetectorController]" << errMsg;
    return false;
}

bool DetectorController::syncInvoke(int cmdId, const QVariantList& args, int timeoutMs, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    if (timeoutMs < 0) {
        errMsg = QString("SyncInvoke, cmd=%1 failed: invalid timeout %2")
        .arg(cmdId)
            .arg(timeoutMs);
        return false;
    }

    /* SyncInvoke 与 Invoke 共用同一套参数降级逻辑，只是最终调用的 SDK API 不同。 */
    std::vector<IRayCmdObject> cmdArgs;
    cmdArgs.reserve(args.size());

    for (const QVariant& v : args) {
        switch (v.typeId()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Bool:
            cmdArgs.emplace_back(v.toInt());
            break;

        case QMetaType::Float:
        case QMetaType::Double:
            cmdArgs.emplace_back(v.toDouble());
            break;

        case QMetaType::QString:
        case QMetaType::QByteArray:
            cmdArgs.emplace_back(v.toString().toStdString());
            break;

        default:
            errMsg = QString("SyncInvoke, cmd=%1 failed: unsupported QVariant type (%2)")
                         .arg(cmdId)
                         .arg(QString::fromLatin1(v.typeName() ? v.typeName() : "unknown"));
            return false;
        }
    }

    int ret = Err_InvalidParamValue;

    switch (cmdArgs.size()) {
    case 0: ret = m_detector->SyncInvoke(cmdId, timeoutMs); break;
    case 1: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], timeoutMs); break;
    case 2: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], timeoutMs); break;
    case 3: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], timeoutMs); break;
    case 4: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], timeoutMs); break;
    case 5: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], timeoutMs); break;
    case 6: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], cmdArgs[5], timeoutMs); break;
    case 7: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], cmdArgs[5], cmdArgs[6], timeoutMs); break;
    case 8: ret = m_detector->SyncInvoke(cmdId, cmdArgs[0], cmdArgs[1], cmdArgs[2], cmdArgs[3], cmdArgs[4], cmdArgs[5], cmdArgs[6], cmdArgs[7], timeoutMs); break;
    default:
        errMsg = QString("SyncInvoke, cmd=%1 failed: too many parameters (%2), max supported = 8")
                     .arg(cmdId)
                     .arg(cmdArgs.size());
        return false;
    }

    return checkResult(ret, errMsg, QString("SyncInvoke, cmd=%1").arg(cmdId));
}

bool DetectorController::useImageBuffer(quint64 bufSizeBytes, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    int ret = m_detector->UseImageBuf(static_cast<unsigned long long>(bufSizeBytes));
    return checkResult(ret, errMsg, "UseImageBuf");
}

bool DetectorController::clearImageBuffer(QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    int ret = m_detector->ClearImageBuf();
    return checkResult(ret, errMsg, "ClearImageBuf");
}

bool DetectorController::queryImageBufferInfo(ImageBufferInfo& info, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    if (!m_sdk || !m_sdk->IsOpened() || !m_sdk->m_pFnQueryImageBuf) {
        errMsg = "Detector SDK image buffer interface is unavailable.";
        return false;
    }

    /* 每次都重新查询缓冲区快照，避免沿用上一帧过时的宽高和队列数量。 */
    info = ImageBufferInfo{};

    int ret = m_sdk->m_pFnQueryImageBuf(
        m_detector->DetectorID(),
        &info.frameCount,
        &info.height,
        &info.width,
        &info.bytesPerPixel,
        &info.propListMemSize);

    if (ret == Err_ImageBufferEmpty) {
        return true;
    }

    if (!checkResult(ret, errMsg, "QueryImageBuf")) {
        return false;
    }

    if (info.frameCount <= 0)
        return true;

    if (info.width <= 0 || info.height <= 0 || info.bytesPerPixel <= 0) {
        errMsg = QString("Detector returned invalid image metadata: %1x%2, %3 Bpp")
                     .arg(info.width)
                     .arg(info.height)
                     .arg(info.bytesPerPixel);
        return false;
    }

    info.imageSize = info.width * info.height * info.bytesPerPixel;
    return true;
}

bool DetectorController::fetchOneImage(const ImageBufferInfo& info, QString& errMsg)
{
    if (!m_detector) {
        errMsg = "Detector is not initialized.";
        return false;
    }

    if (info.imageSize <= 0 || info.propListMemSize < 0) {
        errMsg = "Invalid image buffer info.";
        return false;
    }

    /* SDK 通过调用方提供的连续内存返回一帧，这里先准备好同尺寸 QByteArray。 */
    QByteArray image;
    image.resize(info.imageSize);

    int frameNo = -1;
    int ret = m_detector->GetImageFromBuf(image.data(), info.imageSize, info.propListMemSize, frameNo);
    if (!checkResult(ret, errMsg, "GetImageFromBuf")) {
        return false;
    }

    qInfo() << "[DetectorController] fetched one image:"
            << "frameCount =" << info.frameCount
            << ", frameNo =" << frameNo
            << ", imageSize =" << info.imageSize
            << ", width =" << info.width
            << ", height =" << info.height
            << ", bytesPerPixel =" << info.bytesPerPixel
            << ", propListMemSize =" << info.propListMemSize;

    if (m_imageCallback) {
        m_imageCallback(image, info.imageSize, frameNo, info.width, info.height, info.bytesPerPixel);
    }

    return true;
}

void DetectorController::setErrorCallback(ErrorCallback cb)
{
    m_errorCallback = std::move(cb);
}

void DetectorController::setEventCallback(EventCallback cb)
{
    m_eventCallback = std::move(cb);
}

void DetectorController::setImageCallback(ImageCallback cb)
{
    m_imageCallback = std::move(cb);
}

bool DetectorController::isInitialized() const
{
    return m_initialized;
}

bool DetectorController::isConnected() const
{
    return m_connected;
}

void DetectorController::UserCallbackHandler(int nDetectorID,
                                             int nEventID,
                                             int nEventLevel,
                                             const char* pszMsg,
                                             int nParam1,
                                             int nParam2,
                                             int nPtrParamLen,
                                             void* pParam)
{
    Q_UNUSED(nPtrParamLen);
    Q_UNUSED(pParam);

    /* SDK 回调可能来自非 Qt 线程，这里只做轻量解码和转发，不直接碰 UI。 */
    const QString msg = pszMsg ? QString::fromLocal8Bit(pszMsg) : QString();

    qInfo() << "[DetectorController] SDK callback:"
            << "detectorId =" << nDetectorID
            << ", eventId =" << nEventID
            << ", eventLevel =" << nEventLevel
            << ", msg =" << msg
            << ", param1 =" << nParam1
            << ", param2 =" << nParam2;

    if (m_eventCallback) {
        m_eventCallback(nDetectorID, nEventID, nEventLevel, msg, nParam1, nParam2);
    }

    if (nEventLevel < 0 && m_errorCallback) {
        m_errorCallback(QString("Detector callback error: event=%1, level=%2, msg=%3")
                            .arg(nEventID)
                            .arg(nEventLevel)
                            .arg(msg));
    }
}

QString DetectorController::fpdErrorToString(int code) const
{
    return QString("FPD error code = %1").arg(code);
}

bool DetectorController::checkResult(int ret, QString& errMsg, const QString& action) const
{
    if (ret == Err_OK) {
        qInfo() << "[DetectorController]" << action << "success";
        return true;
    }

    errMsg = QString("%1 failed: %2").arg(action, fpdErrorToString(ret));
    qWarning() << "[DetectorController]" << errMsg;
    return false;
}
