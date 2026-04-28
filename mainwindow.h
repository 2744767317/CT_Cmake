#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <array>

#include <QMainWindow>
#include <QFileDialog>
#include <QImage>
#include <QMessageBox>
#include <QPixmap>
#include <QThread>
#include <QVector>

#include "acquisitionsaveworker.h"
#include "workdirmanager.h"
#include "xraycontroller.h"
#include "xrayworker.h"
#include "motorworker.h"
#include "detectorworker.h"
#include "reconworker.h"
#include "imageviewer.h"
#include "histogramwidget.h"

class QResizeEvent;
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QWidget;

QT_BEGIN_NAMESPACE
namespace Ui {
class HNUCT;
}
QT_END_NAMESPACE

// 主窗口总控层。
// 探测器、电机、X 光机、采集存盘、重建都在各自线程里工作；
// MainWindow 负责界面状态、线程生命周期以及跨模块信号连接。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

public slots:
    void on_btnSelectWorkDir_clicked();

    void on_btnMotorConnect_clicked();

    void on_btnMotorDisconnect_clicked();

    void on_cbReconAlgm_activated(int index);

    void on_cbAppMode_activated(int index);

    void on_btnDetectorConnect_clicked();

    void on_btnDetectorDisconnect_clicked();

    void on_btnChangeAppMode_clicked();

    void on_btnSelectXRayExePath_clicked();

    void on_btnXRayConnect_clicked();

    void on_btnXRayDisconnect_clicked();

    void on_btnXRayApplySettings_clicked();

    void on_btnXRayOpen_clicked();

    void on_btnXRayClose_clicked();

    void on_btnSelectAcquireSaveDir_clicked();

    void on_btnAcquireSingle_clicked();

    void on_btnAcquireStart_clicked();

    void on_btnAcquireStop_clicked();

    void on_cbCorrectionOffset_toggled(bool checked);

    void on_cbCorrectionGain_toggled(bool checked);

    void on_cbCorrectionDefect_toggled(bool checked);

    // 可视化页面相关槽函数
    void on_btnVisLoadImage_clicked();
    void on_btnVisSaveImage_clicked();
    void on_dSpinVisLevel_valueChanged(double value);
    void on_dSpinVisWidth_valueChanged(double value);
    void on_sliderVisLevel_valueChanged(int value);
    void on_sliderVisWidth_valueChanged(int value);
    void on_sliderVisZoom_valueChanged(int value);
    void on_spinVisZoom_valueChanged(int value);
    void on_sliderVisSlice_valueChanged(int value);
    void on_spinVisSlice_valueChanged(int value);
    void on_cbVisHistogramMode_currentIndexChanged(int index);
    void on_btnVisLoadTestImage_clicked();
    void on_cbVisBackgroundColor_currentIndexChanged(int index);
    void onThemeSelectionChanged(int index);

signals:
    void openXRay();
    void closeXRay();
    void applyXRaySettings(double kv, int ua);
    void saveAcquiredFrame(const QString& sessionDir,
                           int sequenceNo,
                           const QByteArray& image,
                           int width,
                           int height,
                           int bytesPerPixel);

private slots:
    void on_btnReconEx_clicked();

    void on_cbFDKFoV_checkStateChanged(const Qt::CheckState &arg1);

    void on_cbSARTFoV_checkStateChanged(const Qt::CheckState &arg1);

    void on_btnReconCancel_clicked();

    void on_btnSelectProjFolder_clicked();

    void on_btSelectReconSavePath_clicked();

    void onDetectorInitialized(bool ok);
    void onDetectorCurrentCorrectOptionRead(int option);
    void onDetectorCurrentCorrectOptionApplied(int option);
    void onDetectorAcquisitionStateChanged(bool running, bool continuous);
    void onDetectorImageReady(const QByteArray& image,
                              int imageSize,
                              int frameNo,
                              int width,
                              int height,
                              int bytesPerPixel);
    void onAcquireFrameSaved(int sequenceNo, const QString& filePath);
    void onXRayInitialized(bool ok);
    void onXRayConnectionChanged(bool connected);
    void onXRayStateChanged(bool hvOn);
    void onXRayStatusUpdated(const XRayController::Status& status);
    void onMotorConnectedChanged(bool connected);
    void onMotorStatusUpdated(const QVector<float>& pos,
                              const QVector<float>& actualPos,
                              const QVector<float>& spd,
                              const QVector<qint32>& runningStates,
                              const QVector<quint32>& homeState);
    void onMotorOperationFinished(const QString& opName, bool ok, const QString& message);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    static constexpr int kMotorAxisCount = 6;

    // 一行电机轴控件的缓存。
    // 这样刷新状态时可以按“轴索引”统一更新，而不用写 6 套重复代码。
    struct MotorAxisWidgets
    {
        QLabel* commandPosValue = nullptr;
        QLabel* actualPosValue = nullptr;
        QLabel* speedValue = nullptr;
        QLabel* runningValue = nullptr;
        QLabel* homeValue = nullptr;
        QDoubleSpinBox* absPosSpin = nullptr;
        QDoubleSpinBox* relDistSpin = nullptr;
        QDoubleSpinBox* velocitySpin = nullptr;
        QDoubleSpinBox* accelerationSpin = nullptr;
        QPushButton* seekZeroButton = nullptr;
        QPushButton* cancelSeekZeroButton = nullptr;
        QPushButton* absMoveButton = nullptr;
        QPushButton* relMoveButton = nullptr;
        QPushButton* stopButton = nullptr;
        QPushButton* emergencyStopButton = nullptr;
    };

    // 采集页面的粗粒度状态机。
    // 这个状态机只描述 UI 层语义，不直接等价于底层 SDK 的全部状态。
    enum class AcquireState
    {
        Disconnected,
        Idle,
        StartingSingle,
        Single,
        StartingContinuous,
        Continuous,
        Stopping
    };

    // -------------------------
    // UI 初始化 / 线程生命周期 / 页面辅助函数
    // -------------------------
    void refreshSerialPorts();
    void initializeMotorUi();
    void initializeAcquireUi();
    void initializeXRayUi();
    void initializeVisualizationPage();
    void initializeVisualizationWidgets();
    void connectVisualizationSignals();
    void initializeThemeControls();
    void applyUiTheme(bool darkTheme);
    QString buildAppStyleSheet(bool darkTheme) const;
    void loadVisualizationImage();
    void syncVisualizationControlsFromViewer();
    void syncVisualizationWindowLevelControls(int level, int width);
    void applyVisualizationWindowLevel(int level, int width);
    void applyVisualizationSlice(int slice);
    void applyVisualizationZoom(int zoom);
    bool ensureMotorWorkerThread();
    void cleanupMotorThread(bool invokeShutdown);
    void setAcquireState(AcquireState state);
    void updateMotorButtons();
    void updateAcquireButtons();
    void updateXRayButtons();
    void setMotorOperationStatusText(const QString& text);
    void resetMotorStatusUi();
    void requestMotorSeekZero(int axisId);
    void requestMotorCancelSeekZero(int axisId);
    void requestMotorAbsMove(int axisId);
    void requestMotorRelMove(int axisId);
    void requestMotorStop(int axisId);
    void requestMotorEmergencyStop(int axisId);
    bool validateMotorSeekZero(int axisId,
                               double velocity,
                               double acceleration,
                               QString* error) const;
    bool validateMotorMove(int axisId,
                           bool relativeMove,
                           double targetOrDistance,
                           double velocity,
                           double acceleration,
                           QString* error) const;
    bool validateXRaySettings(double kv,
                              int ua,
                              bool requireEmission,
                              QString* error) const;
    bool canOpenXRay(QString* error) const;
    bool validateReconstructionInputs(QString* error) const;
    void updateAcquireStatusText(const QString& text);
    void updateAcquireFrameInfoText(int frameNo, int width, int height, int bytesPerPixel);
    void resetXRayStatusUi();
    void updateXRayStatusUi(const XRayController::Status& status);
    void updateCorrectionUi(int option);
    void requestCurrentCorrectOption();
    void applyCorrectionSelection();
    QString formatCorrectionSummary(int option) const;
    QString createAcquireSessionDir();
    void clearAcquireSession();
    void clearAcquirePreview();
    QImage buildPreviewImage(const QByteArray& image, int width, int height, int bytesPerPixel) const;
    void updatePreviewPixmap();
    bool startDetectorAcquisition(bool continuous);
    void stopDetectorAcquisition(bool waitForStop);
    void connectDetectorWorkerSignals();
    void cleanupXRayThread(bool invokeShutdown);
    bool parseXRayEndpoint(QString* host, quint16* port) const;
    void connectXRayWorkerSignals();
    bool detectorCanAcquire() const;
    bool correctionControlsEnabled() const;
    ReconWorker::Task buildReconstructionTask() const;
    void startReconstructionTask(const ReconWorker::Task& task);

private:
    Ui::HNUCT *ui;

    // 每个耗时子系统各占一个线程，避免任何硬件 I/O 或重建任务卡住主界面。
    QThread* m_detectorThread = nullptr;
    DetectorWorker* m_detectorWorker = nullptr;

    QThread* m_saveThread = nullptr;
    AcquisitionSaveWorker* m_saveWorker = nullptr;

    QThread* m_motorThread = nullptr;
    MotorWorker* m_motorWorker = nullptr;
    bool m_motorConnected = false;
    bool m_motorConnecting = false;
    QLabel* m_motorConnectionStatusValueLabel = nullptr;
    QLabel* m_motorOperationStatusValueLabel = nullptr;
    QWidget* m_motorAxesContainer = nullptr;
    std::array<MotorAxisWidgets, kMotorAxisCount> m_motorAxisWidgets;
    std::array<float, kMotorAxisCount> m_lastMotorCommandPos{};
    std::array<float, kMotorAxisCount> m_lastMotorActualPos{};
    std::array<qint32, kMotorAxisCount> m_lastMotorRunningStates{};
    bool m_hasMotorStatusSnapshot = false;

    QThread* m_xrayThread = nullptr;
    XRayWorker* m_xrayWorker = nullptr;

    QString workdir = "";
    WorkdirManager* m_workdirManager = nullptr;

    ApplicationMode m_currentMode;

    QThread* m_reconThread = nullptr;
    ReconWorker* m_reconWorker = nullptr;

    AcquireState m_acquireState = AcquireState::Disconnected;
    bool m_detectorReady = false;
    bool m_updatingCorrectionUi = false;
    bool m_correctionUpdatePending = false;
    bool m_xrayConnecting = false;
    bool m_xrayConnected = false;
    bool m_xrayHvOn = false;
    int m_currentCorrectOption = Enm_CorrectOp_Null;
    QString m_acquireSessionDir;
    int m_nextAcquireSequenceNo = 1;
    QImage m_lastPreviewImage;
    XRayController::Status m_lastXRayStatus;


    // 可视化页面相关成员变量
    ImageViewer* m_imageViewer = nullptr;
    // 图像查看器负责切片显示，直方图控件负责窗宽窗位联动与分布展示。
    HistogramWidget* m_histogramWidget = nullptr;
    double m_visWindowLevel = 128.0;
    double m_visWindowWidth = 255.0;
    QComboBox* m_themeComboBox = nullptr;
    bool m_darkThemeEnabled = true;
};
#endif // MAINWINDOW_H
