#ifndef WORKDIRMANAGER_H
#define WORKDIRMANAGER_H

#include <QObject>
#include <QDir>
#include <QSettings>
#include <QVector>
#include <QComboBox>
#include <QFileSystemWatcher>

// Parsed detector application mode entry from DynamicApplicationMode.ini.
struct ApplicationMode {
    int Index;
    int FullWell;
    int Binning;
    int Frequency;
    int ExposureMode;
    QString ROIRange;
    QString subset;
    int ImageWidth;
    int ImageHeight;
};

// Reads the detector work directory, validates application modes, and keeps the UI in sync
// when DynamicApplicationMode.ini is edited externally.
class WorkdirManager : public QObject
{
    Q_OBJECT

public:
    explicit WorkdirManager(QString dir);
    ~WorkdirManager();

    int CheckWorkdir();
    void ParseApplicationModeInfo();
    void initComboBox(QComboBox* comboBox);

    const QVector<ApplicationMode>& getAppModes() const { return appmode_info; }

signals:
    // Emitted after the INI file is reparsed so the UI can rebuild its mode list.
    // 当 INI 文件在外部被修改并重新解析完毕后，触发此信号
    void applicationModesChanged();

private slots:
    // React to out-of-process edits of DynamicApplicationMode.ini.
    // 处理文件变化的槽函数
    void onIniFileChanged(const QString &path);

private:
    bool ValidateApplicationMode(ApplicationMode& mode);

private:
    QDir workdir;
    QSettings* config_file = nullptr;
    QSettings* appmode_file = nullptr;

    QVector<ApplicationMode> appmode_info;

    // 文件监控器器指针
    // Watches DynamicApplicationMode.ini for external edits.
    QFileSystemWatcher* m_fileWatcher = nullptr;
};

#endif // WORKDIRMANAGER_H
