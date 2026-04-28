#include "workdirmanager.h"
#include <QDebug>
#include <QVariant>
#include <QRegularExpression>
#include <QFile>

WorkdirManager::WorkdirManager(QString dir)
    : QObject(nullptr)
    , workdir(dir)
{
    // 实例化文件监控器并连接信号
    // The watcher lets the UI react when detector configuration is edited outside the app.
    m_fileWatcher = new QFileSystemWatcher(this);
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, this, &WorkdirManager::onIniFileChanged);
}


WorkdirManager::~WorkdirManager()
{
    if (config_file) {
        delete config_file;
        config_file = nullptr;
    }
    if (appmode_file) {
        delete appmode_file;
        appmode_file = nullptr;
    }
    if (m_fileWatcher) {
        delete m_fileWatcher;
        m_fileWatcher = nullptr;
    }
}

int WorkdirManager::CheckWorkdir()
{
    // This method validates the directory shape once and prepares the QSettings readers that
    // the rest of the UI depends on for detector mode metadata.
    // 1. 检查必要的文件是否存在
    if (!workdir.exists("config.ini") || !workdir.exists("DynamicApplicationMode.ini")) {
        qWarning() << "Missing required ini files in:" << workdir.absolutePath();
        return -1;
    }

    // 2. 检查并创建必要的子目录
    if (!workdir.exists("Correct"))
        workdir.mkdir("Correct");
    if (!workdir.exists("Others"))
        workdir.mkdir("Others");

    // 3. 实例化 QSettings 用于读取配置文件
    QString configPath = workdir.absoluteFilePath("config.ini");
    QString appmodePath = workdir.absoluteFilePath("DynamicApplicationMode.ini");

    config_file = new QSettings(configPath, QSettings::IniFormat);
    appmode_file = new QSettings(appmodePath, QSettings::IniFormat);

    // 4. 解析探测器模式信息
    // Reparse before notifying the UI so slots can read the new mode list immediately.
    ParseApplicationModeInfo();
    if (!m_fileWatcher->files().contains(appmodePath)) {
        m_fileWatcher->addPath(appmodePath);
    }
    return 0;
}

void WorkdirManager::onIniFileChanged(const QString &path)
{
    qInfo() << "External INI modification detected in:" << path;

    // 注意：某些外部编辑器（如 Notepad++）在保存文件时，是先删除原文件再创建一个同名新文件。
    // 这种“原子保存”操作会导致 QFileSystemWatcher 丢失对该文件的监控。
    // 因此我们需要检查并重新将其加入监控列表。
    if (!m_fileWatcher->files().contains(path) && QFile::exists(path)) {
        m_fileWatcher->addPath(path);
    }

    // 重新解析数据
    ParseApplicationModeInfo();

    // 发送信号通知 UI 界面去刷新 ComboBox
    emit applicationModesChanged();
}


void WorkdirManager::ParseApplicationModeInfo()
{
    if (appmode_file)
    {
        // 【关键新增】：QSettings 默认会将 INI 内容缓存在内存中。
        // 文件在外部被修改时，必须调用 sync() 强制清空内存缓存并重新读取磁盘。
        appmode_file->sync();

        appmode_info.clear();

        QStringList groups = appmode_file->childGroups();
        appmode_info.reserve(groups.size());

        for (int i = 0; i < groups.size(); i++)
        {
            ApplicationMode mode = { 0 };
            mode.Index = i;

            QString group = QString("ApplicationMode%1").arg(i + 1);
            appmode_file->beginGroup(group);

            mode.FullWell = appmode_file->value("FullWell").toInt();
            mode.Binning = appmode_file->value("Binning").toInt();
            mode.Frequency = appmode_file->value("Frequency").toInt();
            mode.ExposureMode = appmode_file->value("ExposureMode").toInt();
            // 先转为 StringList，再用逗号拼接回来，完美还原 "(0,70,2339,2882)"
            mode.ROIRange = appmode_file->value("ROIRange").toStringList().join(",");
            mode.subset = appmode_file->value("subset").toString();

            appmode_file->endGroup();

            if (ValidateApplicationMode(mode)) {
                appmode_info.push_back(mode);
            } else {
                qWarning() << "Mode" << mode.Index + 1 << "failed validation and was ignored.";
            }
        }

        qDebug() << "Parsed" << appmode_info.size() << "Valid Application Modes.";
    }
}

bool WorkdirManager::ValidateApplicationMode(ApplicationMode& mode)
{
    // 1. 检查 Binning 限制 (取值范围: 0, 1)
    if (mode.Binning != 0 && mode.Binning != 1) {
        qWarning() << "Invalid Binning value:" << mode.Binning << "in Mode" << mode.Index + 1;
        return false;
    }

    // 2. 检查 Frequency 限制 (与 Binning 强相关)
    if (mode.Frequency <= 0) {
        qWarning() << "Invalid Frequency:" << mode.Frequency << "in Mode" << mode.Index + 1;
        return false;
    }
    if (mode.Binning == 0 && mode.Frequency > 18) {
        // Binning 0 (1x1) 时，最大帧率为 18 fps
        qWarning() << "Frequency exceeds Max 18fps for Binning 0 in Mode" << mode.Index + 1;
        return false;
    }
    if (mode.Binning == 1 && mode.Frequency > 36) {
        // Binning 1 (2x2) 时，最大帧率为 36 fps
        qWarning() << "Frequency exceeds Max 36fps for Binning 1 in Mode" << mode.Index + 1;
        return false;
    }

    // 3. 检查 ExposureMode 限制 (仅支持 129: Continuous, 131: Flush)
    if (mode.ExposureMode != 129 && mode.ExposureMode != 131) {
        qWarning() << "Invalid ExposureMode for NDT0506PHS:" << mode.ExposureMode << "in Mode" << mode.Index + 1;
        return false;
    }

    // 4. 解析并检查 ROIRange 限制
    QRegularExpression rx("\\((\\d+),\\s*(\\d+),\\s*(\\d+),\\s*(\\d+)\\)");
    QRegularExpressionMatch match = rx.match(mode.ROIRange);

    if (match.hasMatch()) {
        int colStart = match.captured(1).toInt();
        int rowStart = match.captured(2).toInt();
        int colEnd   = match.captured(3).toInt();
        int rowEnd   = match.captured(4).toInt();

        if (colStart > colEnd || rowStart > rowEnd) {
            qWarning() << "Invalid ROI coordinates order in Mode" << mode.Index + 1;
            return false;
        }

        mode.ImageWidth = (colEnd - colStart + 1) / (mode.Binning + 1);
        mode.ImageHeight = (rowEnd - rowStart + 1) / (mode.Binning + 1);
        // 计算物理图像宽度 (列数)

        // NDT0506PHS 专属限制 1：图像宽度不能小于 256
        if (mode.ImageWidth < 256) {
            qWarning() << "Image width must be >= 256. Calculated:" << mode.ImageWidth << "in Mode" << mode.Index + 1;
            return false;
        }
        int roiCols = colEnd - colStart + 1;
        // NDT0506PHS 专属限制 2：总 ROI 列数必须是 16 的倍数
        if (roiCols % 16 != 0 && roiCols != 2340) {
            qWarning() << "Total ROI columns is not a multiple of 16. Current:" << roiCols
                       << "in Mode" << mode.Index + 1 << "(SDK might pad the image automatically).";
        }
        // 这里我们去掉 return false; 让它继续往下走，保护全画幅设置
    } else {
        qWarning() << "Invalid ROIRange format:" << mode.ROIRange << "in Mode" << mode.Index + 1;
        return false;
    }

    return true;
}



void WorkdirManager::initComboBox(QComboBox* comboBox)
{
    if (!comboBox) return;

    // 断开 ComboBox 原有的信号连接，防止在 clear() 或重绘时触发误操作
    comboBox->blockSignals(true);

    comboBox->clear();

    for (int i = 0; i < appmode_info.size(); i++) {
        const ApplicationMode& mode = appmode_info[i];

        QString expModeStr = "Unknown";
        if (mode.ExposureMode == 129) expModeStr = "Continuous";
        else if (mode.ExposureMode == 131) expModeStr = "Flush";

        QString fwStr = (mode.FullWell == 0) ? "HFW(0)" : "LFW(1)";

        QString displayText = QString("Mode %1: %2 | %3x%4 | Bin:%5x%8 | %6fps | FW:%7")
                                  .arg(mode.Index + 1)
                                  .arg(expModeStr)
                                  .arg(mode.ImageWidth)    // 绑定 %3
                                  .arg(mode.ImageHeight)   // 绑定 %4
                                  .arg(mode.Binning+1)       // 绑定 %5
                                  .arg(mode.Frequency)     // 绑定 %6
                                  .arg(fwStr)                          // 绑定 %7
                                  .arg(mode.Binning+1) ;

        comboBox->addItem(displayText, QVariant(i));
    }

    // 恢复信号响应
    comboBox->blockSignals(false);
}
