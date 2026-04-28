#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "logger.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QComboBox>
#include <QMetaObject>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QResizeEvent>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVariantList>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cmath>
#include <limits>

namespace
{
constexpr int kExpectedDetectorBytesPerPixel = 2;
constexpr int kMotorAxisCountConst = 6;
constexpr double kMotorMinVelocity = 0.001;
constexpr double kMotorMinAcceleration = 0.0;
constexpr double kXRayMaxVoltageKv = 160.0;
constexpr int kXRayProtocolMaxCurrentUa = 999999;
constexpr double kInferredTubePowerW = 320.0;
constexpr int kXRayReadyFaceState = 0x0001;
constexpr int kXRayHvOnFaceState = 0x0004;
constexpr int kXRayFaultFaceState = 0x0007;
constexpr int kXRayReadySystemMode = 1;
constexpr int kXRayNormalSystemMode = 10;
constexpr int kXRayFaultSystemMode = 11;
constexpr int kXRayBlockingInterlockMask = 0x0033;

struct MotorAxisSafetyConfig
{
    const char* label;
    double minPosition;
    double maxPosition;
    double maxVelocity;
    double maxAcceleration;
};

const std::array<MotorAxisSafetyConfig, kMotorAxisCountConst> kMotorAxisSafetyConfigs = {{
    {"X", -1000.0, 1000.0, 200.0, 1000.0},
    {"Y", -1000.0, 1000.0, 200.0, 1000.0},
    {"Z", -1000.0, 1000.0, 200.0, 1000.0},
    {"A", -360.0, 360.0, 180.0, 720.0},
    {"B", -360.0, 360.0, 180.0, 720.0},
    {"C", -360.0, 360.0, 180.0, 720.0},
}};

const MotorAxisSafetyConfig& motorAxisSafetyConfig(int axisId)
{
    Q_ASSERT(axisId >= 0 && axisId < kMotorAxisCountConst);
    return kMotorAxisSafetyConfigs[static_cast<std::size_t>(axisId)];
}

bool xrayHasFault(const XRayController::Status& status)
{
    return status.valid &&
           (status.pcFaultCode != 0 ||
            status.controllerFaultCode != 0 ||
            status.faceState == kXRayFaultFaceState ||
            status.systemMode == kXRayFaultSystemMode);
}

bool xrayHasBlockingInterlock(const XRayController::Status& status)
{
    return status.valid && (status.faceBitData & kXRayBlockingInterlockMask) != 0;
}

QString motorAxisName(int axisId)
{
    static const QStringList axisNames = {
        QStringLiteral("X"),
        QStringLiteral("Y"),
        QStringLiteral("Z"),
        QStringLiteral("A"),
        QStringLiteral("B"),
        QStringLiteral("C")
    };

    if (axisId >= 0 && axisId < axisNames.size()) {
        return axisNames.at(axisId);
    }

    return QStringLiteral("Axis%1").arg(axisId);
}

QString formatMotorFloat(float value)
{
    return QString::number(value, 'f', 3);
}

QString motorRunningText(qint32 runningState)
{
    return runningState != 0 ? QStringLiteral("运行中")
                             : QStringLiteral("停止");
}

QString formatMotorHomeState(quint32 homeState)
{
    return QStringLiteral("0x%1")
        .arg(homeState, 8, 16, QLatin1Char('0'))
        .toUpper();
}

QString formatHex16(int value)
{
    return QStringLiteral("0x%1")
        .arg(value & 0xFFFF, 4, 16, QLatin1Char('0'))
        .toUpper();
}

QString formatKvDeci(int kvDeci)
{
    return QStringLiteral("%1 kV").arg(QString::number(kvDeci / 10.0, 'f', 1));
}

QString formatUa(int ua)
{
    return QStringLiteral("%1 uA").arg(ua);
}

QString formatCentiMaAsUa(int centiMa)
{
    return formatUa(qRound(static_cast<double>(centiMa) * 10.0));
}

QString xrayFaceStateText(int state)
{
    static const QStringList states = {
        QStringLiteral("DC-DC\u5145\u7535"),
        QStringLiteral("\u51C6\u5907\u5B8C\u6BD5"),
        QStringLiteral("\u9AD8\u538B\u9884\u8B66"),
        QStringLiteral("\u6B63\u5728\u5347\u538B"),
        QStringLiteral("\u9AD8\u538B\u5F00\u542F"),
        QStringLiteral("\u6B63\u5728\u5173\u9AD8\u538B"),
        QStringLiteral("\u7B49\u5F85 DC \u4E0B\u964D"),
        QStringLiteral("\u5C04\u7EBF\u673A\u6545\u969C"),
        QStringLiteral("\u706F\u4E1D\u6D4B\u8BD5"),
        QStringLiteral("DC 测试"),
        QStringLiteral("kV 测试"),
        QStringLiteral("\u672A\u77E5\u72B6\u6001")
    };

    if (state >= 0 && state < states.size()) {
        return states.at(state);
    }

    return QStringLiteral("\u672A\u77E5\u72B6\u6001(%1)").arg(formatHex16(state));
}

QString xraySystemModeText(int mode)
{
    static const QStringList modes = {
        QStringLiteral("\u6B63\u5728\u62BD\u771F\u7A7A"),
        QStringLiteral("\u51C6\u5907\u597D\u72B6\u6001"),
        QStringLiteral("\u81EA\u52A8\u8BAD\u7BA1"),
        QStringLiteral("\u8BAD\u7BA1\u6210\u529F"),
        QStringLiteral("\u8BAD\u7BA1\u4E2D\u65AD"),
        QStringLiteral("\u706F\u4E1D\u81EA\u52A8\u6821\u51C6"),
        QStringLiteral("\u706F\u4E1D\u6821\u51C6\u6210\u529F"),
        QStringLiteral("\u5355\u805A\u4E2D"),
        QStringLiteral("\u5168\u805A\u4E2D"),
        QStringLiteral("\u5347\u538B\u8BAD\u7BA1"),
        QStringLiteral("\u6B63\u5E38\u5DE5\u4F5C\u72B6\u6001"),
        QStringLiteral("\u6545\u969C\u72B6\u6001")
    };

    if (mode >= 0 && mode < modes.size()) {
        return modes.at(mode);
    }

    return QStringLiteral("\u672A\u77E5\u6A21\u5F0F(%1)").arg(formatHex16(mode));
}

QString xrayInterlockText(int faceBitData)
{
    const bool flowLow = (faceBitData & 0x0001) != 0;
    const bool vacuumLow = (faceBitData & 0x0002) != 0;
    const bool door1Open = (faceBitData & 0x0010) != 0;
    const bool door2Open = (faceBitData & 0x0020) != 0;

    return QStringLiteral("\u6D41\u91CF%1 | \u771F\u7A7A%2 | \u95E81%3 | \u95E82%4 | Raw=%5")
        .arg(flowLow ? QStringLiteral("\u4E0D\u8DB3") : QStringLiteral("\u6B63\u5E38"))
        .arg(vacuumLow ? QStringLiteral("\u4E0D\u8DB3") : QStringLiteral("\u6B63\u5E38"))
        .arg(door1Open ? QStringLiteral("\u6253\u5F00") : QStringLiteral("\u5173\u95ED"))
        .arg(door2Open ? QStringLiteral("\u6253\u5F00") : QStringLiteral("\u5173\u95ED"))
        .arg(formatHex16(faceBitData));
}

QString motorRangeText(const MotorAxisSafetyConfig& config)
{
    return QStringLiteral("[%1, %2]")
        .arg(QString::number(config.minPosition, 'f', 3))
        .arg(QString::number(config.maxPosition, 'f', 3));
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::HNUCT)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("医学图像软件"));
    initializeThemeControls();

    /*
     * 采集落盘固定放到独立线程里，避免保存大尺寸原始图像时阻塞主界面。
     * MainWindow 只负责发出保存请求，不直接执行磁盘 I/O。
     */
    m_saveThread = new QThread(this);
    m_saveWorker = new AcquisitionSaveWorker();
    m_saveWorker->moveToThread(m_saveThread);

    connect(m_saveThread, &QThread::finished,
            m_saveWorker, &QObject::deleteLater);
    connect(this, &MainWindow::saveAcquiredFrame,
            m_saveWorker, &AcquisitionSaveWorker::saveFrame,
            Qt::QueuedConnection);
    connect(m_saveWorker, &AcquisitionSaveWorker::saveFailed, this,
            [this](const QString& msg) {
                qWarning() << "[AcquireSaveWorker]" << msg;
                updateAcquireStatusText(QStringLiteral("图像保存失败"));
            });
    connect(m_saveWorker, &AcquisitionSaveWorker::frameSaved,
            this, &MainWindow::onAcquireFrameSaved);

    m_saveThread->start();

    // 初始状态下，重建取消按钮没有意义，等真正启动重建后再启用。
    // 协作式停止：只发停止请求，不阻塞 UI 线程等待重建线程退出。
    /*
     * 初始状态下没有重建任务在跑，因此取消按钮保持禁用。
     * 取消逻辑采用协作式停止，只发出停止请求，不直接终止线程。
     */
    ui->btnReconCancel->setEnabled(false);
    refreshSerialPorts();
    initializeMotorUi();
    initializeAcquireUi();
    initializeXRayUi();
    initializeVisualizationPage();
    applyUiTheme(m_darkThemeEnabled);
}

MainWindow::~MainWindow()
{
    on_btnDetectorDisconnect_clicked();
    on_btnXRayDisconnect_clicked();
    on_btnMotorDisconnect_clicked();

    if (m_saveThread) {
        m_saveThread->quit();
        m_saveThread->wait();
        m_saveThread = nullptr;
        m_saveWorker = nullptr;
    }

    if (m_workdirManager) {
        m_workdirManager->deleteLater();
        m_workdirManager = nullptr;
    }

    delete ui;
}

void MainWindow::initializeThemeControls()
{
    auto* cornerWidget = new QWidget(ui->tabWidget);
    cornerWidget->setObjectName(QStringLiteral("themeCornerWidget"));

    auto* layout = new QHBoxLayout(cornerWidget);
    layout->setContentsMargins(8, 6, 8, 0);
    layout->setSpacing(8);

    auto* label = new QLabel(QStringLiteral("主题"), cornerWidget);
    label->setObjectName(QStringLiteral("themeCornerLabel"));

    m_themeComboBox = new QComboBox(cornerWidget);
    m_themeComboBox->setObjectName(QStringLiteral("themeComboBox"));
    m_themeComboBox->addItem(QStringLiteral("暗黑"));
    m_themeComboBox->addItem(QStringLiteral("亮白"));
    m_themeComboBox->setCurrentIndex(m_darkThemeEnabled ? 0 : 1);
    m_themeComboBox->setMinimumWidth(112);

    layout->addWidget(label);
    layout->addWidget(m_themeComboBox);

    ui->tabWidget->setCornerWidget(cornerWidget, Qt::TopRightCorner);

    connect(m_themeComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onThemeSelectionChanged);
}

QString MainWindow::buildAppStyleSheet(bool darkTheme) const
{
    if (darkTheme) {
        return QStringLiteral(
            "QMainWindow, QWidget#centralwidget {"
            "background-color: #0b1220;"
            "}"
            "QWidget {"
            "color: #e2e8f0;"
            "font-family: 'Microsoft YaHei UI';"
            "}"
            "QWidget#themeCornerWidget {"
            "background: transparent;"
            "}"
            "QLabel#themeCornerLabel {"
            "color: #cbd5e1;"
            "font-weight: 600;"
            "}"
            "QTabWidget::pane {"
            "border: 1px solid #1f2a3a;"
            "background: #0b1220;"
            "border-radius: 12px;"
            "top: -1px;"
            "}"
            "QTabBar::tab {"
            "background: #111827;"
            "color: #93a4ba;"
            "border: 1px solid #233044;"
            "border-bottom: none;"
            "border-top-left-radius: 10px;"
            "border-top-right-radius: 10px;"
            "padding: 10px 22px;"
            "margin-right: 4px;"
            "}"
            "QTabBar::tab:selected {"
            "background: #162033;"
            "color: #f8fafc;"
            "}"
            "QTabBar::tab:hover:!selected {"
            "background: #152238;"
            "color: #dbeafe;"
            "}"
            "QGroupBox {"
            "background: #111827;"
            "border: 1px solid #233044;"
            "border-radius: 12px;"
            "margin-top: 16px;"
            "padding-top: 12px;"
            "font-weight: 600;"
            "}"
            "QGroupBox::title {"
            "subcontrol-origin: margin;"
            "left: 14px;"
            "padding: 0 6px;"
            "color: #f8fafc;"
            "}"
            "QLabel {"
            "color: #dbe5f2;"
            "background: transparent;"
            "}"
            "QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox, QDoubleSpinBox {"
            "background: #0f172a;"
            "color: #f8fafc;"
            "border: 1px solid #334155;"
            "border-radius: 8px;"
            "padding: 6px 10px;"
            "selection-background-color: #38bdf8;"
            "}"
            "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {"
            "border: 1px solid #38bdf8;"
            "}"
            "QComboBox::drop-down, QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {"
            "border: none;"
            "background: transparent;"
            "width: 20px;"
            "}"
            "QPushButton {"
            "background: #172136;"
            "color: #f8fafc;"
            "border: 1px solid #334155;"
            "border-radius: 8px;"
            "padding: 7px 14px;"
            "}"
            "QPushButton:hover {"
            "background: #1e293b;"
            "border-color: #475569;"
            "}"
            "QPushButton:pressed {"
            "background: #0f172a;"
            "}"
            "QPushButton:disabled {"
            "background: #111827;"
            "color: #64748b;"
            "border-color: #1f2937;"
            "}"
            "QCheckBox {"
            "spacing: 8px;"
            "}"
            "QCheckBox::indicator {"
            "width: 16px;"
            "height: 16px;"
            "border-radius: 4px;"
            "border: 1px solid #475569;"
            "background: #0f172a;"
            "}"
            "QCheckBox::indicator:checked {"
            "background: #0ea5e9;"
            "border-color: #38bdf8;"
            "}"
            "QSlider::groove:horizontal {"
            "height: 6px;"
            "background: #1f2937;"
            "border-radius: 3px;"
            "}"
            "QSlider::sub-page:horizontal {"
            "background: #0ea5e9;"
            "border-radius: 3px;"
            "}"
            "QSlider::handle:horizontal {"
            "width: 14px;"
            "margin: -5px 0;"
            "background: #f8fafc;"
            "border: 1px solid #38bdf8;"
            "border-radius: 7px;"
            "}"
            "QScrollBar:vertical {"
            "background: #0f172a;"
            "width: 10px;"
            "margin: 0;"
            "}"
            "QScrollBar::handle:vertical {"
            "background: #334155;"
            "border-radius: 5px;"
            "min-height: 28px;"
            "}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
            "background: none;"
            "border: none;"
            "}"
            "QWidget#widgetVisImageViewer, QWidget#widgetVisHistogram {"
            "background: #09101c;"
            "border: 1px solid #1f2a3a;"
            "border-radius: 12px;"
            "}"
            "QToolTip {"
            "background: #111827;"
            "color: #f8fafc;"
            "border: 1px solid #334155;"
            "padding: 4px 8px;"
            "}");
    }

    return QStringLiteral(
        "QMainWindow, QWidget#centralwidget {"
        "background-color: #edf2f8;"
        "}"
        "QWidget {"
        "color: #0f172a;"
        "font-family: 'Microsoft YaHei UI';"
        "}"
        "QWidget#themeCornerWidget {"
        "background: transparent;"
        "}"
        "QLabel#themeCornerLabel {"
        "color: #334155;"
        "font-weight: 600;"
        "}"
        "QTabWidget::pane {"
        "border: 1px solid #d6deea;"
        "background: #edf2f8;"
        "border-radius: 12px;"
        "top: -1px;"
        "}"
        "QTabBar::tab {"
        "background: #dfe7f1;"
        "color: #475569;"
        "border: 1px solid #cbd5e1;"
        "border-bottom: none;"
        "border-top-left-radius: 10px;"
        "border-top-right-radius: 10px;"
        "padding: 10px 22px;"
        "margin-right: 4px;"
        "}"
        "QTabBar::tab:selected {"
        "background: #ffffff;"
        "color: #0f172a;"
        "}"
        "QTabBar::tab:hover:!selected {"
        "background: #e8eef7;"
        "color: #1d4ed8;"
        "}"
        "QGroupBox {"
        "background: #ffffff;"
        "border: 1px solid #d6deea;"
        "border-radius: 12px;"
        "margin-top: 16px;"
        "padding-top: 12px;"
        "font-weight: 600;"
        "}"
        "QGroupBox::title {"
        "subcontrol-origin: margin;"
        "left: 14px;"
        "padding: 0 6px;"
        "color: #0f172a;"
        "}"
        "QLabel {"
        "color: #334155;"
        "background: transparent;"
        "}"
        "QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox, QDoubleSpinBox {"
        "background: #f8fafc;"
        "color: #0f172a;"
        "border: 1px solid #cbd5e1;"
        "border-radius: 8px;"
        "padding: 6px 10px;"
        "selection-background-color: #60a5fa;"
        "}"
        "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {"
        "border: 1px solid #2563eb;"
        "}"
        "QComboBox::drop-down, QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {"
        "border: none;"
        "background: transparent;"
        "width: 20px;"
        "}"
        "QPushButton {"
        "background: #ffffff;"
        "color: #0f172a;"
        "border: 1px solid #cbd5e1;"
        "border-radius: 8px;"
        "padding: 7px 14px;"
        "}"
        "QPushButton:hover {"
        "background: #eff6ff;"
        "border-color: #93c5fd;"
        "}"
        "QPushButton:pressed {"
        "background: #dbeafe;"
        "}"
        "QPushButton:disabled {"
        "background: #f1f5f9;"
        "color: #94a3b8;"
        "border-color: #d6deea;"
        "}"
        "QCheckBox {"
        "spacing: 8px;"
        "}"
        "QCheckBox::indicator {"
        "width: 16px;"
        "height: 16px;"
        "border-radius: 4px;"
        "border: 1px solid #94a3b8;"
        "background: #ffffff;"
        "}"
        "QCheckBox::indicator:checked {"
        "background: #2563eb;"
        "border-color: #2563eb;"
        "}"
        "QSlider::groove:horizontal {"
        "height: 6px;"
        "background: #dbe4f0;"
        "border-radius: 3px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "background: #2563eb;"
        "border-radius: 3px;"
        "}"
        "QSlider::handle:horizontal {"
        "width: 14px;"
        "margin: -5px 0;"
        "background: #ffffff;"
        "border: 1px solid #2563eb;"
        "border-radius: 7px;"
        "}"
        "QScrollBar:vertical {"
        "background: #f1f5f9;"
        "width: 10px;"
        "margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "background: #cbd5e1;"
        "border-radius: 5px;"
        "min-height: 28px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        "background: none;"
        "border: none;"
        "}"
        "QWidget#widgetVisImageViewer, QWidget#widgetVisHistogram {"
        "background: #ffffff;"
        "border: 1px solid #d6deea;"
        "border-radius: 12px;"
        "}"
        "QToolTip {"
        "background: #ffffff;"
        "color: #0f172a;"
        "border: 1px solid #cbd5e1;"
        "padding: 4px 8px;"
        "}");
}

void MainWindow::applyUiTheme(bool darkTheme)
{
    m_darkThemeEnabled = darkTheme;

    qApp->setStyleSheet(buildAppStyleSheet(darkTheme));

    if (m_themeComboBox) {
        const QSignalBlocker blocker(m_themeComboBox);
        m_themeComboBox->setCurrentIndex(darkTheme ? 0 : 1);
    }

    if (m_imageViewer) {
        m_imageViewer->setInterfaceTheme(darkTheme);
    }
    if (m_histogramWidget) {
        m_histogramWidget->setDarkTheme(darkTheme);
    }
}

void MainWindow::onThemeSelectionChanged(int index)
{
    applyUiTheme(index == 0);
}

void MainWindow::refreshSerialPorts()
{
    ui->comboMotorPort->clear();

    const auto ports = QSerialPortInfo::availablePorts();

    if (ports.empty())
    {
        ui->comboMotorPort->addItem("No serial port");
        return;
    }

    for (const QSerialPortInfo &info : ports)
    {
        QString displayName = info.portName();
        if (!info.description().isEmpty())
            displayName += " (" + info.description() + ")";

        ui->comboMotorPort->addItem(displayName, info.portName());
    }
}

void MainWindow::initializeMotorUi()
{
    qRegisterMetaType<QVector<float>>("QVector<float>");
    qRegisterMetaType<QVector<qint32>>("QVector<qint32>");
    qRegisterMetaType<QVector<quint32>>("QVector<quint32>");

    m_motorConnectionStatusValueLabel = ui->lbMotorConnectionStatusValue;
    m_motorOperationStatusValueLabel = ui->lbMotorOperationStatusValue;
    m_motorAxesContainer = ui->motorAxesContainer;

    QFont compactFont = ui->groupBox_2->font();
    compactFont.setPointSize(10);

    QFont headerFont = compactFont;
    headerFont.setBold(true);

    auto* axesLayout = new QGridLayout(m_motorAxesContainer);
    axesLayout->setContentsMargins(4, 4, 4, 4);
    axesLayout->setHorizontalSpacing(8);
    axesLayout->setVerticalSpacing(6);

    const QStringList headers = {
        QStringLiteral("Axis"),
        QStringLiteral("Cmd Pos"),
        QStringLiteral("Act Pos"),
        QStringLiteral("Speed"),
        QStringLiteral("Running"),
        QStringLiteral("Home"),
        QStringLiteral("Abs Target"),
        QStringLiteral("Rel Dist"),
        QStringLiteral("Velocity"),
        QStringLiteral("Accel"),
        QStringLiteral("Actions")
    };

    for (int column = 0; column < headers.size(); ++column) {
        auto* headerLabel = new QLabel(headers.at(column), m_motorAxesContainer);
        headerLabel->setAlignment(Qt::AlignCenter);
        headerLabel->setFont(headerFont);
        axesLayout->addWidget(headerLabel, 0, column);
    }

    auto createStatusLabel = [&](const QString& text) {
        auto* label = new QLabel(text, m_motorAxesContainer);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(88);
        label->setFont(compactFont);
        return label;
    };

    auto createSpinBox = [&](double minimum, double maximum, double value) {
        auto* spinBox = new QDoubleSpinBox(m_motorAxesContainer);
        spinBox->setRange(minimum, maximum);
        spinBox->setDecimals(3);
        spinBox->setSingleStep(0.1);
        spinBox->setValue(value);
        spinBox->setAlignment(Qt::AlignRight);
        spinBox->setMinimumWidth(92);
        spinBox->setFont(compactFont);
        return spinBox;
    };

    auto createButton = [&](const QString& text) {
        auto* button = new QPushButton(text, m_motorAxesContainer);
        button->setFont(compactFont);
        button->setMinimumWidth(62);
        return button;
    };

    for (int axisId = 0; axisId < kMotorAxisCount; ++axisId) {
        const int row = axisId + 1;
        auto& axisWidgets = m_motorAxisWidgets[axisId];
        const auto& safetyConfig = motorAxisSafetyConfig(axisId);
        const double relativeRange = safetyConfig.maxPosition - safetyConfig.minPosition;

        auto* axisNameLabel = new QLabel(motorAxisName(axisId), m_motorAxesContainer);
        axisNameLabel->setAlignment(Qt::AlignCenter);
        axisNameLabel->setFont(headerFont);

        axisWidgets.commandPosValue = createStatusLabel(QStringLiteral("--"));
        axisWidgets.actualPosValue = createStatusLabel(QStringLiteral("--"));
        axisWidgets.speedValue = createStatusLabel(QStringLiteral("--"));
        axisWidgets.runningValue = createStatusLabel(QStringLiteral("Offline"));
        axisWidgets.homeValue = createStatusLabel(QStringLiteral("--"));
        axisWidgets.absPosSpin = createSpinBox(safetyConfig.minPosition, safetyConfig.maxPosition, 0.0);
        axisWidgets.relDistSpin = createSpinBox(-relativeRange, relativeRange, 0.0);
        axisWidgets.velocitySpin = createSpinBox(kMotorMinVelocity, safetyConfig.maxVelocity, 1.0);
        axisWidgets.accelerationSpin = createSpinBox(kMotorMinAcceleration, safetyConfig.maxAcceleration, 0.0);

        axisWidgets.absPosSpin->setToolTip(
            QStringLiteral("Software range %1").arg(motorRangeText(safetyConfig)));
        axisWidgets.relDistSpin->setToolTip(
            QStringLiteral("Projected target stays inside %1").arg(motorRangeText(safetyConfig)));
        axisWidgets.velocitySpin->setToolTip(
            QStringLiteral("Allowed range: %1 ~ %2")
                .arg(QString::number(kMotorMinVelocity, 'f', 3))
                .arg(QString::number(safetyConfig.maxVelocity, 'f', 3)));
        axisWidgets.accelerationSpin->setToolTip(
            QStringLiteral("Allowed range: %1 ~ %2")
                .arg(QString::number(kMotorMinAcceleration, 'f', 3))
                .arg(QString::number(safetyConfig.maxAcceleration, 'f', 3)));

        auto* actionsWidget = new QWidget(m_motorAxesContainer);
        auto* actionsLayout = new QHBoxLayout(actionsWidget);
        actionsLayout->setContentsMargins(0, 0, 0, 0);
        actionsLayout->setSpacing(4);

        axisWidgets.seekZeroButton = createButton(QStringLiteral("Home"));
        axisWidgets.cancelSeekZeroButton = createButton(QStringLiteral("Cancel"));
        axisWidgets.absMoveButton = createButton(QStringLiteral("Abs Move"));
        axisWidgets.relMoveButton = createButton(QStringLiteral("Rel Move"));
        axisWidgets.stopButton = createButton(QStringLiteral("Stop"));
        axisWidgets.emergencyStopButton = createButton(QStringLiteral("E-Stop"));

        actionsLayout->addWidget(axisWidgets.seekZeroButton);
        actionsLayout->addWidget(axisWidgets.cancelSeekZeroButton);
        actionsLayout->addWidget(axisWidgets.absMoveButton);
        actionsLayout->addWidget(axisWidgets.relMoveButton);
        actionsLayout->addWidget(axisWidgets.stopButton);
        actionsLayout->addWidget(axisWidgets.emergencyStopButton);

        axesLayout->addWidget(axisNameLabel, row, 0);
        axesLayout->addWidget(axisWidgets.commandPosValue, row, 1);
        axesLayout->addWidget(axisWidgets.actualPosValue, row, 2);
        axesLayout->addWidget(axisWidgets.speedValue, row, 3);
        axesLayout->addWidget(axisWidgets.runningValue, row, 4);
        axesLayout->addWidget(axisWidgets.homeValue, row, 5);
        axesLayout->addWidget(axisWidgets.absPosSpin, row, 6);
        axesLayout->addWidget(axisWidgets.relDistSpin, row, 7);
        axesLayout->addWidget(axisWidgets.velocitySpin, row, 8);
        axesLayout->addWidget(axisWidgets.accelerationSpin, row, 9);
        axesLayout->addWidget(actionsWidget, row, 10);

        connect(axisWidgets.seekZeroButton, &QPushButton::clicked, this,
                [this, axisId]() { requestMotorSeekZero(axisId); });
        connect(axisWidgets.cancelSeekZeroButton, &QPushButton::clicked, this,
                [this, axisId]() { requestMotorCancelSeekZero(axisId); });
        connect(axisWidgets.absMoveButton, &QPushButton::clicked, this,
                [this, axisId]() { requestMotorAbsMove(axisId); });
        connect(axisWidgets.relMoveButton, &QPushButton::clicked, this,
                [this, axisId]() { requestMotorRelMove(axisId); });
        connect(axisWidgets.stopButton, &QPushButton::clicked, this,
                [this, axisId]() { requestMotorStop(axisId); });
        connect(axisWidgets.emergencyStopButton, &QPushButton::clicked, this,
                [this, axisId]() { requestMotorEmergencyStop(axisId); });
    }

    axesLayout->setColumnStretch(10, 1);
    m_motorAxesContainer->setMinimumWidth(1360);
    m_motorConnectionStatusValueLabel->setText(QStringLiteral("Not connected"));
    setMotorOperationStatusText(QStringLiteral("Idle"));
    resetMotorStatusUi();
    updateMotorButtons();
}

void MainWindow::initializeAcquireUi()
{
    ui->lbAcquirePreview->setAlignment(Qt::AlignCenter);
    ui->lbAcquirePreview->setText(QStringLiteral("暂无图像"));
    ui->lbAcquirePreview->setScaledContents(false);

    ui->leAcquireSaveDir->setReadOnly(true);
    ui->leAcquireSessionDir->setReadOnly(true);

    clearAcquirePreview();
    clearAcquireSession();

    ui->lbAcquireCorrectionStatus->setText(QStringLiteral("当前校正: 未知"));
    updateAcquireFrameInfoText(-1, m_currentMode.ImageWidth, m_currentMode.ImageHeight, kExpectedDetectorBytesPerPixel);
    setAcquireState(AcquireState::Disconnected);
    updateAcquireStatusText(QStringLiteral("探测器未连接"));
}

void MainWindow::initializeXRayUi()
{
    ui->dSpinXRayVoltage->setRange(0.0, kXRayMaxVoltageKv);
    ui->dSpinXRayVoltage->setDecimals(1);
    ui->dSpinXRayVoltage->setSingleStep(0.1);
    ui->spinXRayCurrentUa->setRange(0, kXRayProtocolMaxCurrentUa);
    ui->spinXRayCurrentUa->setSingleStep(10);
    ui->dSpinXRayVoltage->setToolTip(
        QStringLiteral("Tube voltage range: 0 ~ %1 kV").arg(kXRayMaxVoltageKv, 0, 'f', 1));
    ui->spinXRayCurrentUa->setToolTip(
        QStringLiteral("Protocol field allows 0 ~ %1 uA; power guard uses an inferred %2 W envelope.")
            .arg(kXRayProtocolMaxCurrentUa)
            .arg(QString::number(kInferredTubePowerW, 'f', 0)));

    resetXRayStatusUi();
    ui->lbXRayConnectionStatusValue->setText(QStringLiteral("未连接"));
    updateXRayButtons();
}

bool MainWindow::ensureMotorWorkerThread()
{
    if (m_motorThread && m_motorWorker) {
        return true;
    }

    cleanupMotorThread(false);

    m_motorThread = new QThread(this);
    m_motorWorker = new MotorWorker();
    m_motorWorker->moveToThread(m_motorThread);

    connect(m_motorThread, &QThread::finished,
            m_motorWorker, &QObject::deleteLater);
    connect(m_motorWorker, &MotorWorker::connectedChanged,
            this, &MainWindow::onMotorConnectedChanged);
    connect(m_motorWorker, &MotorWorker::statusUpdated,
            this, &MainWindow::onMotorStatusUpdated);
    connect(m_motorWorker, &MotorWorker::operationFinished,
            this, &MainWindow::onMotorOperationFinished);
    connect(m_motorWorker, &MotorWorker::errorOccurred, this,
            [this](const QString& title, const QString& message) {
                QMessageBox::critical(this,
                                      title.isEmpty() ? QStringLiteral("Motor Error")
                                                      : title,
                                      message);
            });

    m_motorThread->start();
    return true;
}

void MainWindow::cleanupMotorThread(bool invokeShutdown)
{
    if (!m_motorThread) {
        m_motorWorker = nullptr;
        return;
    }

    if (invokeShutdown && m_motorWorker && m_motorThread->isRunning()) {
        QMetaObject::invokeMethod(m_motorWorker, "shutdown",
                                  Qt::BlockingQueuedConnection);
    }

    if (m_motorThread->isRunning()) {
        m_motorThread->quit();
        m_motorThread->wait();
    }

    m_motorThread->deleteLater();
    m_motorThread = nullptr;
    m_motorWorker = nullptr;
}

void MainWindow::setAcquireState(AcquireState state)
{
    m_acquireState = state;
    updateAcquireButtons();
}

void MainWindow::updateMotorButtons()
{
    const bool hasWorkerThread = (m_motorThread != nullptr && m_motorWorker != nullptr);
    const bool canConnect = !m_motorConnecting && !m_motorConnected;
    const bool canDisconnect = hasWorkerThread && !m_motorConnecting;

    ui->comboMotorPort->setEnabled(canConnect);
    ui->btnMotorConnect->setEnabled(canConnect);
    ui->btnMotorDisconnect->setEnabled(canDisconnect);

    for (auto& axisWidgets : m_motorAxisWidgets) {
        const bool canControl = m_motorConnected && !m_motorConnecting;
        if (axisWidgets.absPosSpin) {
            axisWidgets.absPosSpin->setEnabled(canControl);
        }
        if (axisWidgets.relDistSpin) {
            axisWidgets.relDistSpin->setEnabled(canControl);
        }
        if (axisWidgets.velocitySpin) {
            axisWidgets.velocitySpin->setEnabled(canControl);
        }
        if (axisWidgets.accelerationSpin) {
            axisWidgets.accelerationSpin->setEnabled(canControl);
        }
        if (axisWidgets.seekZeroButton) {
            axisWidgets.seekZeroButton->setEnabled(canControl);
        }
        if (axisWidgets.cancelSeekZeroButton) {
            axisWidgets.cancelSeekZeroButton->setEnabled(canControl);
        }
        if (axisWidgets.absMoveButton) {
            axisWidgets.absMoveButton->setEnabled(canControl);
        }
        if (axisWidgets.relMoveButton) {
            axisWidgets.relMoveButton->setEnabled(canControl);
        }
        if (axisWidgets.stopButton) {
            axisWidgets.stopButton->setEnabled(canControl);
        }
        if (axisWidgets.emergencyStopButton) {
            axisWidgets.emergencyStopButton->setEnabled(canControl);
        }
    }
}

bool MainWindow::validateMotorSeekZero(int axisId,
                                       double velocity,
                                       double acceleration,
                                       QString* error) const
{
    if (axisId < 0 || axisId >= kMotorAxisCount) {
        if (error) {
            *error = QStringLiteral("Invalid axis index.");
        }
        return false;
    }

    const auto& config = motorAxisSafetyConfig(axisId);
    if (!m_hasMotorStatusSnapshot) {
        if (error) {
            *error = QStringLiteral("Motor status is not available yet. Wait for the first status refresh.");
        }
        return false;
    }

    const double speedMagnitude = std::abs(velocity);
    if (speedMagnitude < kMotorMinVelocity || speedMagnitude > config.maxVelocity) {
        if (error) {
            *error = QStringLiteral("Axis %1 home speed must stay within [%2, %3].")
                         .arg(config.label)
                         .arg(QString::number(kMotorMinVelocity, 'f', 3))
                         .arg(QString::number(config.maxVelocity, 'f', 3));
        }
        return false;
    }

    if (acceleration < kMotorMinAcceleration || acceleration > config.maxAcceleration) {
        if (error) {
            *error = QStringLiteral("Axis %1 acceleration must stay within [%2, %3].")
                         .arg(config.label)
                         .arg(QString::number(kMotorMinAcceleration, 'f', 3))
                         .arg(QString::number(config.maxAcceleration, 'f', 3));
        }
        return false;
    }

    if (m_hasMotorStatusSnapshot && m_lastMotorRunningStates[axisId] != 0) {
        if (error) {
            *error = QStringLiteral("Axis %1 is still moving. Stop it before homing.")
                         .arg(config.label);
        }
        return false;
    }

    return true;
}

bool MainWindow::validateMotorMove(int axisId,
                                   bool relativeMove,
                                   double targetOrDistance,
                                   double velocity,
                                   double acceleration,
                                   QString* error) const
{
    if (axisId < 0 || axisId >= kMotorAxisCount) {
        if (error) {
            *error = QStringLiteral("Invalid axis index.");
        }
        return false;
    }

    const auto& config = motorAxisSafetyConfig(axisId);
    if (!m_hasMotorStatusSnapshot) {
        if (error) {
            *error = QStringLiteral("Motor status is not available yet. Wait for the first status refresh.");
        }
        return false;
    }

    if (velocity < kMotorMinVelocity || velocity > config.maxVelocity) {
        if (error) {
            *error = QStringLiteral("Axis %1 velocity must stay within [%2, %3].")
                         .arg(config.label)
                         .arg(QString::number(kMotorMinVelocity, 'f', 3))
                         .arg(QString::number(config.maxVelocity, 'f', 3));
        }
        return false;
    }

    if (acceleration < kMotorMinAcceleration || acceleration > config.maxAcceleration) {
        if (error) {
            *error = QStringLiteral("Axis %1 acceleration must stay within [%2, %3].")
                         .arg(config.label)
                         .arg(QString::number(kMotorMinAcceleration, 'f', 3))
                         .arg(QString::number(config.maxAcceleration, 'f', 3));
        }
        return false;
    }

    if (m_hasMotorStatusSnapshot && m_lastMotorRunningStates[axisId] != 0) {
        if (error) {
            *error = QStringLiteral("Axis %1 is still moving. Wait for it to stop before sending another move.")
                         .arg(config.label);
        }
        return false;
    }

    if (relativeMove) {
        if (std::abs(targetOrDistance) < 1e-6) {
            if (error) {
                *error = QStringLiteral("Relative distance cannot be zero.");
            }
            return false;
        }

        const double projectedTarget = static_cast<double>(m_lastMotorActualPos[axisId]) + targetOrDistance;
        if (projectedTarget < config.minPosition || projectedTarget > config.maxPosition) {
            if (error) {
                *error = QStringLiteral("Axis %1 projected target %2 is outside the software range %3.")
                             .arg(config.label)
                             .arg(QString::number(projectedTarget, 'f', 3))
                             .arg(motorRangeText(config));
            }
            return false;
        }

        return true;
    }

    if (targetOrDistance < config.minPosition || targetOrDistance > config.maxPosition) {
        if (error) {
            *error = QStringLiteral("Axis %1 target %2 is outside the software range %3.")
                         .arg(config.label)
                         .arg(QString::number(targetOrDistance, 'f', 3))
                         .arg(motorRangeText(config));
        }
        return false;
    }

    return true;
}

bool MainWindow::validateXRaySettings(double kv,
                                      int ua,
                                      bool requireEmission,
                                      QString* error) const
{
    if (kv < 0.0 || kv > kXRayMaxVoltageKv) {
        if (error) {
            *error = QStringLiteral("Tube voltage must stay within [0, %1] kV.")
                         .arg(QString::number(kXRayMaxVoltageKv, 'f', 1));
        }
        return false;
    }

    if (ua < 0 || ua > kXRayProtocolMaxCurrentUa) {
        if (error) {
            *error = QStringLiteral("Tube current must stay within [0, %1] uA.")
                         .arg(kXRayProtocolMaxCurrentUa);
        }
        return false;
    }

    if (requireEmission && kv <= 0.0) {
        if (error) {
            *error = QStringLiteral("Set a positive tube voltage before turning high voltage on.");
        }
        return false;
    }

    if (requireEmission && ua <= 0) {
        if (error) {
            *error = QStringLiteral("Set a positive tube current before turning high voltage on.");
        }
        return false;
    }

    if (kv <= 0.0 && ua > 0) {
        if (error) {
            *error = QStringLiteral("Tube current cannot be non-zero when tube voltage is zero.");
        }
        return false;
    }

    const double powerW = kv * static_cast<double>(ua) / 1000.0;
    if (powerW > kInferredTubePowerW + 1e-6) {
        if (error) {
            *error = QStringLiteral("Requested operating point %1 W exceeds the inferred %2 W tube envelope.")
                         .arg(powerW, 0, 'f', 1)
                         .arg(kInferredTubePowerW, 0, 'f', 0);
        }
        return false;
    }

    return true;
}

bool MainWindow::canOpenXRay(QString* error) const
{
    if (!m_xrayWorker || !m_xrayConnected) {
        if (error) {
            *error = QStringLiteral("X-ray source is not connected.");
        }
        return false;
    }

    if (!m_lastXRayStatus.valid) {
        if (error) {
            *error = QStringLiteral("No valid X-ray status frame has been received yet.");
        }
        return false;
    }

    QString settingError;
    if (!validateXRaySettings(ui->dSpinXRayVoltage->value(),
                              ui->spinXRayCurrentUa->value(),
                              true,
                              &settingError)) {
        if (error) {
            *error = settingError;
        }
        return false;
    }

    if (xrayHasFault(m_lastXRayStatus)) {
        if (error) {
            *error = QStringLiteral("X-ray source reports a fault: PC=%1, Controller=%2.")
                         .arg(formatHex16(m_lastXRayStatus.pcFaultCode))
                         .arg(formatHex16(m_lastXRayStatus.controllerFaultCode));
        }
        return false;
    }

    if (xrayHasBlockingInterlock(m_lastXRayStatus)) {
        if (error) {
            *error = QStringLiteral("X-ray interlock is active: %1")
                         .arg(xrayInterlockText(m_lastXRayStatus.faceBitData));
        }
        return false;
    }

    if (m_lastXRayStatus.systemMode != kXRayReadySystemMode &&
        m_lastXRayStatus.systemMode != kXRayNormalSystemMode) {
        if (error) {
            *error = QStringLiteral("X-ray system mode is not ready for HV on: %1.")
                         .arg(xraySystemModeText(m_lastXRayStatus.systemMode));
        }
        return false;
    }

    if (m_lastXRayStatus.faceState != kXRayReadyFaceState &&
        m_lastXRayStatus.faceState != kXRayHvOnFaceState) {
        if (error) {
            *error = QStringLiteral("X-ray source is not in a ready state: %1.")
                         .arg(xrayFaceStateText(m_lastXRayStatus.faceState));
        }
        return false;
    }

    return true;
}

bool MainWindow::validateReconstructionInputs(QString* error) const
{
    const QString projectionDirPath = ui->leProjFolder->text().trimmed();
    if (projectionDirPath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Projection directory is empty.");
        }
        return false;
    }

    QDir projectionDir(projectionDirPath);
    if (!projectionDir.exists()) {
        if (error) {
            *error = QStringLiteral("Projection directory does not exist: %1").arg(projectionDirPath);
        }
        return false;
    }

    const QStringList projectionFiles =
        projectionDir.entryList(QStringList() << "*.mha" << "*.MHA", QDir::Files);
    if (projectionFiles.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Projection directory does not contain any .mha files.");
        }
        return false;
    }

    const QString outputPath = ui->leReconSavePath->text().trimmed();
    if (outputPath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Output file path is empty.");
        }
        return false;
    }

    QFileInfo outputInfo(outputPath);
    QDir outputDir = outputInfo.dir();
    if (!outputDir.exists() && !outputDir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("Failed to create output directory: %1")
                         .arg(outputDir.absolutePath());
        }
        return false;
    }

    const int nProj = ui->spinAngleNum->value();
    if (nProj <= 0) {
        if (error) {
            *error = QStringLiteral("Projection count must be greater than zero.");
        }
        return false;
    }

    const double sid = ui->dSpinSID->value();
    const double sdd = ui->dSpinSDD->value();
    if (sid <= 0.0 || sdd <= 0.0) {
        if (error) {
            *error = QStringLiteral("SID and SDD must both be positive.");
        }
        return false;
    }

    if (sdd <= sid) {
        if (error) {
            *error = QStringLiteral("SDD must be greater than SID.");
        }
        return false;
    }

    if (ui->dSpinDetSpacingX->value() <= 0.0 ||
        ui->dSpinDetSpacingY->value() <= 0.0 ||
        ui->dSpinVolSpacingX->value() <= 0.0 ||
        ui->dSpinVolSpacingY->value() <= 0.0 ||
        ui->dSpinVolSpacingZ->value() <= 0.0) {
        if (error) {
            *error = QStringLiteral("Detector and volume spacing values must be positive.");
        }
        return false;
    }

    if (ui->spinDetU->value() <= 0 || ui->spinDetV->value() <= 0 ||
        ui->spinVolX->value() <= 0 || ui->spinVolY->value() <= 0 || ui->spinVolZ->value() <= 0) {
        if (error) {
            *error = QStringLiteral("Detector and volume dimensions must be greater than zero.");
        }
        return false;
    }

    if (ui->dSpinFDKhannCut->value() < 0.0 || ui->dSpinFDKhannCut->value() > 1.0) {
        if (error) {
            *error = QStringLiteral("FDK Hann cut frequency must stay within [0, 1].");
        }
        return false;
    }

    if (ui->dSpinFDKtrucCorrect->value() < 0.0) {
        if (error) {
            *error = QStringLiteral("FDK truncation correction cannot be negative.");
        }
        return false;
    }

    if (ui->cbReconAlgm->currentIndex() == 1) {
        if (ui->spinSARTSubset->value() <= 0 || ui->spinSARTSubset->value() > nProj) {
            if (error) {
                *error = QStringLiteral("SART subset size must stay within [1, projection count].");
            }
            return false;
        }

        if (ui->spinSARTIter->value() <= 0) {
            if (error) {
                *error = QStringLiteral("SART iteration count must be greater than zero.");
            }
            return false;
        }

        if (ui->dSpinSARTLambda->value() <= 0.0) {
            if (error) {
                *error = QStringLiteral("SART lambda must be positive.");
            }
            return false;
        }

        if (ui->spinSARTTVIter->value() > 0 && ui->dSpinSARTTVGamma->value() <= 0.0) {
            if (error) {
                *error = QStringLiteral("TV gamma must be positive when TV iterations are enabled.");
            }
            return false;
        }
    }

    return true;
}

void MainWindow::updateAcquireButtons()
{
    const bool canStart = detectorCanAcquire() &&
                          (m_acquireState == AcquireState::Idle);
    const bool canStop = m_detectorReady &&
                         (m_acquireState == AcquireState::StartingSingle ||
                          m_acquireState == AcquireState::Single ||
                          m_acquireState == AcquireState::StartingContinuous ||
                          m_acquireState == AcquireState::Continuous ||
                          m_acquireState == AcquireState::Stopping);

    ui->btnAcquireSingle->setEnabled(canStart);
    ui->btnAcquireStart->setEnabled(canStart);
    ui->btnAcquireStop->setEnabled(canStop);

    const bool correctionEnabled = correctionControlsEnabled();
    ui->cbCorrectionOffset->setEnabled(correctionEnabled);
    ui->cbCorrectionGain->setEnabled(correctionEnabled);
    ui->cbCorrectionDefect->setEnabled(correctionEnabled);
}

void MainWindow::setMotorOperationStatusText(const QString& text)
{
    if (m_motorOperationStatusValueLabel) {
        m_motorOperationStatusValueLabel->setText(text);
    }
}

void MainWindow::resetMotorStatusUi()
{
    // 清空上一轮轮询快照，避免重新连接前继续拿旧位置/旧运行状态做安全判断。
    m_hasMotorStatusSnapshot = false;
    m_lastMotorCommandPos.fill(0.0f);
    m_lastMotorActualPos.fill(0.0f);
    m_lastMotorRunningStates.fill(0);

    for (auto& axisWidgets : m_motorAxisWidgets) {
        if (axisWidgets.commandPosValue) {
            axisWidgets.commandPosValue->setText(QStringLiteral("--"));
        }
        if (axisWidgets.actualPosValue) {
            axisWidgets.actualPosValue->setText(QStringLiteral("--"));
        }
        if (axisWidgets.speedValue) {
            axisWidgets.speedValue->setText(QStringLiteral("--"));
        }
        if (axisWidgets.runningValue) {
            axisWidgets.runningValue->setText(QStringLiteral("Offline"));
        }
        if (axisWidgets.homeValue) {
            axisWidgets.homeValue->setText(QStringLiteral("--"));
        }
    }
}

void MainWindow::updateXRayButtons()
{
    const bool hasThread = m_xrayThread != nullptr;
    const bool canControl = m_xrayConnected && !m_xrayConnecting;

    ui->btnXRayConnect->setEnabled(!hasThread && !m_xrayConnecting);
    ui->btnXRayDisconnect->setEnabled(canControl);
    ui->btnXRayApplySettings->setEnabled(canControl);
    ui->btnXRayOpen->setEnabled(canControl && !m_xrayHvOn);
    ui->btnXRayClose->setEnabled(canControl && m_xrayHvOn);

    const bool editableConnection = !hasThread && !m_xrayConnecting;
    ui->leXRayExePath->setEnabled(editableConnection);
    ui->btnSelectXRayExePath->setEnabled(editableConnection);
    ui->leXRayNetPort->setEnabled(editableConnection);

    ui->dSpinXRayVoltage->setEnabled(!m_xrayConnecting);
    ui->spinXRayCurrentUa->setEnabled(!m_xrayConnecting);
}

void MainWindow::updateAcquireStatusText(const QString& text)
{
    ui->lbAcquireStatus->setText(text);
}

void MainWindow::updateAcquireFrameInfoText(int frameNo, int width, int height, int bytesPerPixel)
{
    QStringList parts;
    if (frameNo >= 0)
        parts << QStringLiteral("帧号: %1").arg(frameNo);

    if (width > 0 && height > 0)
        parts << QStringLiteral("分辨率: %1 x %2").arg(width).arg(height);
    else if (m_currentMode.ImageWidth > 0 && m_currentMode.ImageHeight > 0)
        parts << QStringLiteral("模式分辨率: %1 x %2").arg(m_currentMode.ImageWidth).arg(m_currentMode.ImageHeight);

    if (bytesPerPixel > 0)
        parts << QStringLiteral("像素字节: %1").arg(bytesPerPixel);

    if (parts.isEmpty())
        parts << QStringLiteral("暂无帧信息");

    ui->lbAcquireFrameInfo->setText(parts.join(" | "));
}

void MainWindow::resetXRayStatusUi()
{
    m_lastXRayStatus = XRayController::Status{};

    ui->lbXRayHvStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRaySystemModeValue->setText(QStringLiteral("未连接"));
    ui->lbXRayFaultStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRayInterlockStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRayBoardStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRaySetStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRayMeasureStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRayTargetStatusValue->setText(QStringLiteral("未连接"));
    ui->lbXRayExposureValue->setText(QStringLiteral("未连接"));
    ui->lbXRayArcStatusValue->setText(QStringLiteral("未连接"));
}

void MainWindow::initializeVisualizationWidgets()
{
    /*
     * 可视化页只在主线程持有 QWidget。
     * ImageViewer 负责图像显示与切片，HistogramWidget 负责灰度分布和窗宽窗位联动。
     */
    m_imageViewer = new ImageViewer(ui->widgetVisImageViewer);
    m_imageViewer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* viewerLayout = new QVBoxLayout(ui->widgetVisImageViewer);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewerLayout->addWidget(m_imageViewer);

    m_histogramWidget = new HistogramWidget(ui->widgetVisHistogram);
    m_histogramWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* histogramLayout = new QVBoxLayout(ui->widgetVisHistogram);
    histogramLayout->setContentsMargins(0, 0, 0, 0);
    histogramLayout->addWidget(m_histogramWidget);

    m_imageViewer->setInterfaceTheme(m_darkThemeEnabled);
    m_histogramWidget->setDarkTheme(m_darkThemeEnabled);

    ui->sliderVisLevel->setRange(-2048, 4096);
    ui->sliderVisLevel->setValue(40);
    ui->sliderVisWidth->setRange(1, 4096);
    ui->sliderVisWidth->setValue(400);
    ui->dSpinVisLevel->setRange(-2048.0, 4096.0);
    ui->dSpinVisWidth->setRange(1.0, 4096.0);
    ui->dSpinVisLevel->setSingleStep(1.0);
    ui->dSpinVisWidth->setSingleStep(1.0);
    ui->spinVisSlice->setRange(0, 0);
    ui->sliderVisSlice->setRange(0, 0);
    ui->cbVisBackgroundColor->setCurrentIndex(0);
}

void MainWindow::connectVisualizationSignals()
{
    connect(m_imageViewer, &ImageViewer::windowLevelChanged, this, [this](int level, int width) {
        if (!m_histogramWidget) {
            return;
        }

        syncVisualizationWindowLevelControls(level, width);
        m_histogramWidget->setWindowLevel(level, width);
    });

    connect(m_imageViewer, &ImageViewer::imageLoaded,
            this, &MainWindow::syncVisualizationControlsFromViewer);
    connect(m_imageViewer, &ImageViewer::viewStateChanged,
            this, &MainWindow::syncVisualizationControlsFromViewer);

    connect(m_histogramWidget, &HistogramWidget::windowLevelChanged, this, [this](int level, int width) {
        if (!m_imageViewer) {
            return;
        }

        applyVisualizationWindowLevel(level, width);
    });
}

void MainWindow::initializeVisualizationPage()
{
    initializeVisualizationWidgets();
    connectVisualizationSignals();
    syncVisualizationControlsFromViewer();
}

void MainWindow::syncVisualizationWindowLevelControls(int level, int width)
{
    m_visWindowLevel = level;
    m_visWindowWidth = width;

    const QSignalBlocker levelSpinBlocker(ui->dSpinVisLevel);
    const QSignalBlocker levelSliderBlocker(ui->sliderVisLevel);
    const QSignalBlocker widthSpinBlocker(ui->dSpinVisWidth);
    const QSignalBlocker widthSliderBlocker(ui->sliderVisWidth);

    ui->dSpinVisLevel->setValue(level);
    ui->sliderVisLevel->setValue(level);
    ui->dSpinVisWidth->setValue(width);
    ui->sliderVisWidth->setValue(width);
}

void MainWindow::syncVisualizationControlsFromViewer()
{
    /*
     * 图像载入后，以 ImageViewer 当前真实状态回写所有可视化控件，
     * 让滑块、SpinBox、直方图和切片页始终与当前激活位面一致。
     */
    if (!m_imageViewer || !m_histogramWidget) {
        return;
    }

    const bool hasImage = m_imageViewer->getImageData() != nullptr;
    const int levelMinimum = hasImage
        ? static_cast<int>(std::floor(m_imageViewer->getScalarMinimum()))
        : -2048;
    const int levelMaximum = hasImage
        ? static_cast<int>(std::ceil(m_imageViewer->getScalarMaximum()))
        : 4096;
    const int widthMaximum = qMax(1, levelMaximum - levelMinimum);
    const int totalSlices = qMax(1, m_imageViewer->getTotalSlices());
    const int currentSlice = qBound(0, m_imageViewer->getCurrentSlice(), totalSlices - 1);
    const int currentZoom = qBound(10, m_imageViewer->getZoomLevel(), 500);

    const QSignalBlocker levelSpinBlocker(ui->dSpinVisLevel);
    const QSignalBlocker levelSliderBlocker(ui->sliderVisLevel);
    const QSignalBlocker widthSpinBlocker(ui->dSpinVisWidth);
    const QSignalBlocker widthSliderBlocker(ui->sliderVisWidth);
    const QSignalBlocker sliceSpinBlocker(ui->spinVisSlice);
    const QSignalBlocker sliceSliderBlocker(ui->sliderVisSlice);
    const QSignalBlocker zoomSpinBlocker(ui->spinVisZoom);
    const QSignalBlocker zoomSliderBlocker(ui->sliderVisZoom);

    ui->dSpinVisLevel->setRange(levelMinimum, levelMaximum);
    ui->sliderVisLevel->setRange(levelMinimum, levelMaximum);
    ui->dSpinVisWidth->setRange(1.0, static_cast<double>(widthMaximum));
    ui->sliderVisWidth->setRange(1, widthMaximum);
    ui->spinVisSlice->setRange(0, totalSlices - 1);
    ui->sliderVisSlice->setRange(0, totalSlices - 1);
    ui->groupBoxVisSlice->setEnabled(hasImage && totalSlices > 1);

    m_visWindowLevel = m_imageViewer->getWindowLevel();
    m_visWindowWidth = m_imageViewer->getWindowWidth();

    ui->dSpinVisLevel->setValue(m_visWindowLevel);
    ui->sliderVisLevel->setValue(m_visWindowLevel);
    ui->dSpinVisWidth->setValue(m_visWindowWidth);
    ui->sliderVisWidth->setValue(m_visWindowWidth);
    ui->spinVisSlice->setValue(currentSlice);
    ui->sliderVisSlice->setValue(currentSlice);
    ui->spinVisZoom->setValue(currentZoom);
    ui->sliderVisZoom->setValue(currentZoom);

    m_histogramWidget->setImageData(m_imageViewer->getImageData());
    m_histogramWidget->setSliceOrientation(m_imageViewer->getCurrentOrientation());
    m_histogramWidget->setCurrentSlice(currentSlice);
    m_histogramWidget->setWindowLevel(static_cast<int>(m_visWindowLevel),
                                      static_cast<int>(m_visWindowWidth));
}

void MainWindow::applyVisualizationWindowLevel(int level, int width)
{
    if (!m_imageViewer || !m_histogramWidget) {
        return;
    }

    syncVisualizationWindowLevelControls(level, width);
    m_imageViewer->setWindowLevelWidth(level, width);
    m_histogramWidget->setWindowLevel(level, width);
}

void MainWindow::applyVisualizationSlice(int slice)
{
    if (!m_imageViewer || !m_histogramWidget) {
        return;
    }

    const int totalSlices = qMax(1, m_imageViewer->getTotalSlices());
    const int clampedSlice = qBound(0, slice, totalSlices - 1);

    const QSignalBlocker spinBlocker(ui->spinVisSlice);
    const QSignalBlocker sliderBlocker(ui->sliderVisSlice);
    ui->spinVisSlice->setValue(clampedSlice);
    ui->sliderVisSlice->setValue(clampedSlice);

    m_imageViewer->setCurrentSlice(clampedSlice);
    m_histogramWidget->setSliceOrientation(m_imageViewer->getCurrentOrientation());
    m_histogramWidget->setCurrentSlice(clampedSlice);
}

void MainWindow::applyVisualizationZoom(int zoom)
{
    if (!m_imageViewer) {
        return;
    }

    const QSignalBlocker spinBlocker(ui->spinVisZoom);
    const QSignalBlocker sliderBlocker(ui->sliderVisZoom);
    ui->spinVisZoom->setValue(zoom);
    ui->sliderVisZoom->setValue(zoom);
    m_imageViewer->setZoomLevel(zoom);
}

void MainWindow::on_btnVisLoadImage_clicked()
{
    loadVisualizationImage();
}

void MainWindow::on_btnVisSaveImage_clicked()
{
    const QString fileName = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("保存图像"),
        QString(),
        QStringLiteral("PNG文件 (*.png);;JPEG文件 (*.jpg *.jpeg);;BMP文件 (*.bmp);;MHD文件 (*.mhd);;NII文件 (*.nii)"));

    if (!fileName.isEmpty() && m_imageViewer) {
        m_imageViewer->saveImage(fileName);
    }
}

void MainWindow::on_dSpinVisLevel_valueChanged(double value)
{
    applyVisualizationWindowLevel(qRound(value),
                                  qRound(m_visWindowWidth));
}

void MainWindow::on_dSpinVisWidth_valueChanged(double value)
{
    applyVisualizationWindowLevel(qRound(m_visWindowLevel),
                                  qRound(value));
}

void MainWindow::on_sliderVisLevel_valueChanged(int value)
{
    applyVisualizationWindowLevel(value,
                                  qRound(m_visWindowWidth));
}

void MainWindow::on_sliderVisWidth_valueChanged(int value)
{
    applyVisualizationWindowLevel(qRound(m_visWindowLevel),
                                  value);
}

void MainWindow::on_sliderVisZoom_valueChanged(int value)
{
    applyVisualizationZoom(value);
}

void MainWindow::on_spinVisZoom_valueChanged(int value)
{
    applyVisualizationZoom(value);
}

void MainWindow::on_sliderVisSlice_valueChanged(int value)
{
    applyVisualizationSlice(value);
}

void MainWindow::on_spinVisSlice_valueChanged(int value)
{
    applyVisualizationSlice(value);
}

void MainWindow::on_cbVisHistogramMode_currentIndexChanged(int index)
{
    if (!m_histogramWidget) {
        return;
    }

    m_histogramWidget->setScaleMode(index == 0
        ? HistogramWidget::ScaleMode::Linear
        : HistogramWidget::ScaleMode::Logarithmic);
}

void MainWindow::on_btnVisLoadTestImage_clicked()
{
    if (m_imageViewer) {
        m_imageViewer->loadTestImage();
    }
}

void MainWindow::on_cbVisBackgroundColor_currentIndexChanged(int index)
{
    if (!m_imageViewer) {
        return;
    }

    QString color = QStringLiteral("black");
    switch (index) {
    case 1:
        color = QStringLiteral("white");
        break;
    case 2:
        color = QStringLiteral("gray");
        break;
    case 3:
        color = QStringLiteral("blue");
        break;
    default:
        break;
    }

    m_imageViewer->setBackgroundColor(color);
}

void MainWindow::loadVisualizationImage()
{
    /*
     * ImageViewer 继承 QWidget，线程亲和性属于主界面线程，
     * 因此文件解析和控件更新都保持在当前线程里完成。
     */
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        tr("Select Medical Image"),
        QString(),
        tr("Medical images (*.nii *.nii.gz *.mha *.mhd);;All files (*.*)"));

    if (fileName.isEmpty()) {
        return;
    }

    ui->leVisImagePath->setText(fileName);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool success = m_imageViewer && m_imageViewer->loadImage(fileName);
    QApplication::restoreOverrideCursor();

    if (!success) {
        QMessageBox::warning(this,
                             tr("Load Image"),
                             tr("Failed to load the selected image.\nSupported formats: NII, NII.GZ, MHA, MHD."));
    }
}

void MainWindow::updateXRayStatusUi(const XRayController::Status& status)
{
    // 这里把底层协议解析出来的状态结构体，统一投影到界面标签文本上。
    // 后续如果要做状态面板拆分，这个函数就是最核心的汇聚点。
    m_lastXRayStatus = status;

    if (!status.valid) {
        resetXRayStatusUi();
        return;
    }

    ui->lbXRayHvStatusValue->setText(
        QStringLiteral("%1 | Raw=%2")
            .arg(xrayFaceStateText(status.faceState))
            .arg(formatHex16(status.faceState)));
    ui->lbXRaySystemModeValue->setText(
        QStringLiteral("%1 | Raw=%2")
            .arg(xraySystemModeText(status.systemMode))
            .arg(formatHex16(status.systemMode)));
    ui->lbXRayFaultStatusValue->setText(
        QStringLiteral("PC=%1 | 控制器=%2")
            .arg(formatHex16(status.pcFaultCode))
            .arg(formatHex16(status.controllerFaultCode)));
    ui->lbXRayInterlockStatusValue->setText(xrayInterlockText(status.faceBitData));
    ui->lbXRayBoardStatusValue->setText(
        QStringLiteral("控制板=%1 | 流量反馈=%2 | 保护触发=%3")
            .arg(formatHex16(status.bitData))
            .arg(status.flow)
            .arg(formatHex16(status.trigger)));
    ui->lbXRaySetStatusValue->setText(
        QStringLiteral("%1 | %2")
            .arg(formatKvDeci(status.setKvDeci))
            .arg(formatCentiMaAsUa(status.setMaCenti)));
    ui->lbXRayMeasureStatusValue->setText(
        QStringLiteral("%1 | %2")
            .arg(formatKvDeci(status.measuredKvDeci))
            .arg(formatCentiMaAsUa(status.measuredMaCenti)));
    ui->lbXRayTargetStatusValue->setText(formatUa(status.readTargetUa));
    ui->lbXRayExposureValue->setText(QStringLiteral("%1 s").arg(status.readTimeSec));
    ui->lbXRayArcStatusValue->setText(
        QStringLiteral("%1 | Raw=%2")
            .arg(status.arcCounter)
            .arg(formatHex16(status.arcCounter)));

    if (!ui->dSpinXRayVoltage->hasFocus()) {
        const QSignalBlocker blockVoltage(ui->dSpinXRayVoltage);
        ui->dSpinXRayVoltage->setValue(status.setKvDeci / 10.0);
    }

    if (!ui->spinXRayCurrentUa->hasFocus()) {
        const QSignalBlocker blockCurrent(ui->spinXRayCurrentUa);
        ui->spinXRayCurrentUa->setValue(qRound(static_cast<double>(status.setMaCenti) * 10.0));
    }
}

void MainWindow::updateCorrectionUi(int option)
{
    m_currentCorrectOption = option;
    m_updatingCorrectionUi = true;

    {
        const QSignalBlocker blockOffset(ui->cbCorrectionOffset);
        const QSignalBlocker blockGain(ui->cbCorrectionGain);
        const QSignalBlocker blockDefect(ui->cbCorrectionDefect);

        ui->cbCorrectionOffset->setChecked((option & CDetector::OFFSETMASK) != 0);
        ui->cbCorrectionGain->setChecked((option & CDetector::GAINMASK) != 0);
        ui->cbCorrectionDefect->setChecked((option & CDetector::DEFECTMASK) != 0);
    }

    m_updatingCorrectionUi = false;
    m_correctionUpdatePending = false;

    ui->lbAcquireCorrectionStatus->setText(formatCorrectionSummary(option));
    updateAcquireButtons();
}

void MainWindow::requestCurrentCorrectOption()
{
    if (!m_detectorWorker || !m_detectorReady)
        return;

    m_correctionUpdatePending = true;
    updateAcquireButtons();

    QMetaObject::invokeMethod(m_detectorWorker, "readCurrentCorrectOption",
                              Qt::QueuedConnection);
}

void MainWindow::applyCorrectionSelection()
{
    if (m_updatingCorrectionUi || !m_detectorWorker || !m_detectorReady)
        return;

    m_correctionUpdatePending = true;
    updateAcquireButtons();

    QMetaObject::invokeMethod(m_detectorWorker, "applyCorrectionSelection",
                              Qt::QueuedConnection,
                              Q_ARG(bool, ui->cbCorrectionOffset->isChecked()),
                              Q_ARG(bool, ui->cbCorrectionGain->isChecked()),
                              Q_ARG(bool, ui->cbCorrectionDefect->isChecked()));
}

QString MainWindow::formatCorrectionSummary(int option) const
{
    QStringList enabled;
    if (option & CDetector::OFFSETMASK)
        enabled << QStringLiteral("Offset");
    if (option & CDetector::GAINMASK)
        enabled << QStringLiteral("Gain");
    if (option & CDetector::DEFECTMASK)
        enabled << QStringLiteral("Defect");

    if (enabled.isEmpty())
        enabled << QStringLiteral("无");

    return QStringLiteral("当前校正: %1 | Raw=0x%2")
        .arg(enabled.join(", "))
        .arg(QString::number(static_cast<quint32>(option), 16).toUpper());
}

QString MainWindow::createAcquireSessionDir()
{
    const QString parentDirPath = ui->leAcquireSaveDir->text().trimmed();
    if (parentDirPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先选择采集保存目录。"));
        return {};
    }

    QDir parentDir(parentDirPath);
    if (!parentDir.exists() && !parentDir.mkpath(".")) {
        QMessageBox::critical(this, QStringLiteral("错误"),
                              QStringLiteral("无法创建采集保存目录: %1").arg(parentDirPath));
        return {};
    }

    QString sessionName = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString sessionDirPath = parentDir.filePath(sessionName);

    int suffix = 1;
    while (QDir(sessionDirPath).exists()) {
        sessionDirPath = parentDir.filePath(QString("%1_%2").arg(sessionName).arg(suffix++));
    }

    if (!parentDir.mkpath(QFileInfo(sessionDirPath).fileName())) {
        QMessageBox::critical(this, QStringLiteral("错误"),
                              QStringLiteral("无法创建采集会话目录: %1").arg(sessionDirPath));
        return {};
    }

    m_acquireSessionDir = sessionDirPath;
    m_nextAcquireSequenceNo = 1;
    ui->leAcquireSessionDir->setText(sessionDirPath);
    return sessionDirPath;
}

void MainWindow::clearAcquireSession()
{
    m_acquireSessionDir.clear();
    m_nextAcquireSequenceNo = 1;
    ui->leAcquireSessionDir->clear();
}

void MainWindow::clearAcquirePreview()
{
    m_lastPreviewImage = QImage();
    ui->lbAcquirePreview->clear();
    ui->lbAcquirePreview->setText(QStringLiteral("暂无图像"));
}

QImage MainWindow::buildPreviewImage(const QByteArray& image,
                                     int width,
                                     int height,
                                     int bytesPerPixel) const
{
    if (width <= 0 || height <= 0 || bytesPerPixel <= 0)
        return {};

    const int pixelCount = width * height;
    const int expectedSize = pixelCount * bytesPerPixel;
    if (image.size() < expectedSize)
        return {};

    if (bytesPerPixel == 1) {
        QImage gray(reinterpret_cast<const uchar*>(image.constData()),
                    width,
                    height,
                    width,
                    QImage::Format_Grayscale8);
        return gray.copy();
    }

    if (bytesPerPixel != 2)
        return {};

    const uchar* src = reinterpret_cast<const uchar*>(image.constData());
    quint16 minValue = std::numeric_limits<quint16>::max();
    quint16 maxValue = 0;

    for (int i = 0; i < pixelCount; ++i) {
        const quint16 value = static_cast<quint16>(src[i * 2]) |
                              (static_cast<quint16>(src[i * 2 + 1]) << 8);
        minValue = qMin(minValue, value);
        maxValue = qMax(maxValue, value);
    }

    QImage preview(width, height, QImage::Format_Grayscale8);
    if (preview.isNull())
        return {};

    const int range = qMax<int>(1, static_cast<int>(maxValue) - static_cast<int>(minValue));
    for (int y = 0; y < height; ++y) {
        uchar* dstLine = preview.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            const quint16 value = static_cast<quint16>(src[index * 2]) |
                                  (static_cast<quint16>(src[index * 2 + 1]) << 8);
            dstLine[x] = static_cast<uchar>((static_cast<int>(value) - static_cast<int>(minValue)) * 255 / range);
        }
    }

    return preview;
}

void MainWindow::updatePreviewPixmap()
{
    if (m_lastPreviewImage.isNull()) {
        ui->lbAcquirePreview->clear();
        ui->lbAcquirePreview->setText(QStringLiteral("暂无图像"));
        return;
    }

    const QPixmap pixmap = QPixmap::fromImage(m_lastPreviewImage);
    ui->lbAcquirePreview->setPixmap(
        pixmap.scaled(ui->lbAcquirePreview->size(),
                      Qt::KeepAspectRatio,
                      Qt::SmoothTransformation));
}

bool MainWindow::startDetectorAcquisition(bool continuous)
{
    if (!detectorCanAcquire()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先连接探测器、选择模式并配置保存目录。"));
        return false;
    }

    if (createAcquireSessionDir().isEmpty()) {
        return false;
    }

    updateAcquireStatusText(continuous
        ? QStringLiteral("正在启动连续采集...")
        : QStringLiteral("正在启动单帧采集..."));
    setAcquireState(continuous
        ? AcquireState::StartingContinuous
        : AcquireState::StartingSingle);

    QMetaObject::invokeMethod(m_detectorWorker,
                              continuous ? "startContinuousAcquisition" : "startSingleAcquisition",
                              Qt::QueuedConnection,
                              Q_ARG(int, m_currentMode.ImageWidth),
                              Q_ARG(int, m_currentMode.ImageHeight),
                              Q_ARG(int, kExpectedDetectorBytesPerPixel));
    return true;
}

void MainWindow::stopDetectorAcquisition(bool waitForStop)
{
    if (!m_detectorWorker || !m_detectorThread)
        return;

    if (m_acquireState == AcquireState::Disconnected ||
        m_acquireState == AcquireState::Idle)
        return;

    setAcquireState(AcquireState::Stopping);
    updateAcquireStatusText(QStringLiteral("正在停止采集..."));

    const Qt::ConnectionType type = waitForStop
        ? Qt::BlockingQueuedConnection
        : Qt::QueuedConnection;

    QMetaObject::invokeMethod(m_detectorWorker, "stopAcquisition", type);
}

bool MainWindow::detectorCanAcquire() const
{
    return m_detectorWorker &&
           m_detectorReady &&
           m_currentMode.ImageWidth > 0 &&
           m_currentMode.ImageHeight > 0 &&
           !ui->leAcquireSaveDir->text().trimmed().isEmpty() &&
           !m_correctionUpdatePending;
}

bool MainWindow::correctionControlsEnabled() const
{
    return m_detectorReady &&
           !m_updatingCorrectionUi &&
           !m_correctionUpdatePending &&
           m_acquireState == AcquireState::Idle;
}

void MainWindow::on_btnSelectWorkDir_clicked()
{
    // 工作目录是探测器配置、校正模板、日志和默认采集输出的根入口。
    // 一旦切换目录，相关 UI 状态和 WorkdirManager 都要重新初始化。
    const QString selectedDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择工作目录"), "");
    if (selectedDir.isEmpty())
        return;

    if (m_detectorWorker)
        on_btnDetectorDisconnect_clicked();

    workdir = selectedDir;
    ui->leWorkDir->setText(workdir);

    if (m_workdirManager) {
        m_workdirManager->deleteLater();
        m_workdirManager = nullptr;
    }

    m_workdirManager = new WorkdirManager(workdir);
    m_workdirManager->setParent(this);

    if (m_workdirManager->CheckWorkdir()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("探测器工作目录不包含配置文件。"));
        m_workdirManager->deleteLater();
        m_workdirManager = nullptr;
        return;
    }

    connect(m_workdirManager, &WorkdirManager::applicationModesChanged, this, [this]() {
        qInfo() << "Application mode ini changed externally.";

        const int oldIndex = ui->cbAppMode->currentIndex();
        m_workdirManager->initComboBox(ui->cbAppMode);

        if (oldIndex >= 0 && oldIndex < ui->cbAppMode->count())
            ui->cbAppMode->setCurrentIndex(oldIndex);
        else if (ui->cbAppMode->count() > 0)
            ui->cbAppMode->setCurrentIndex(0);

        if (ui->cbAppMode->currentIndex() >= 0)
            on_cbAppMode_activated(ui->cbAppMode->currentIndex());
    });

    m_workdirManager->initComboBox(ui->cbAppMode);
    if (ui->cbAppMode->count() > 0)
        ui->cbAppMode->setCurrentIndex(0);

    ui->leAcquireSaveDir->setText(QDir(workdir).filePath("Others"));
    clearAcquireSession();

    const QString logPath = QDir(workdir).filePath("log.txt");
    if (!Logger::init(logPath, true)) {
        fprintf(stderr, "Failed to open log file: %s\n", logPath.toLocal8Bit().constData());
    }

    qInfo() << "App started. log =" << logPath;
}

void MainWindow::connectDetectorWorkerSignals()
{
    /*
     * 探测器线程初始化完成后，MainWindow 只监听结果信号：
     * 状态、校正选项、采集过程和图像回调都统一回到 UI 线程更新界面。
     */
    connect(m_detectorThread, &QThread::finished,
            m_detectorWorker, &QObject::deleteLater);

    connect(m_detectorWorker, &DetectorWorker::initialized,
            this, &MainWindow::onDetectorInitialized);
    connect(m_detectorWorker, &DetectorWorker::currentCorrectOptionRead,
            this, &MainWindow::onDetectorCurrentCorrectOptionRead);
    connect(m_detectorWorker, &DetectorWorker::currentCorrectOptionApplied,
            this, &MainWindow::onDetectorCurrentCorrectOptionApplied);
    connect(m_detectorWorker, &DetectorWorker::acquisitionStateChanged,
            this, &MainWindow::onDetectorAcquisitionStateChanged);
    connect(m_detectorWorker, &DetectorWorker::imageReady,
            this, &MainWindow::onDetectorImageReady);
    connect(m_detectorWorker, &DetectorWorker::errorOccurred, this,
            [this](const QString& msg) {
                QMessageBox::critical(this, QStringLiteral("探测器错误"), msg);
            });
}

void MainWindow::on_btnDetectorConnect_clicked()
{
    if (m_detectorWorker)
        return;

    if (workdir.isEmpty() || !m_workdirManager) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先选择有效的探测器工作目录。"));
        return;
    }

    m_detectorThread = new QThread(this);
    m_detectorWorker = new DetectorWorker();
    m_detectorWorker->moveToThread(m_detectorThread);
    connectDetectorWorkerSignals();

    m_detectorThread->start();

    updateAcquireStatusText(QStringLiteral("正在连接探测器..."));
    setAcquireState(AcquireState::Disconnected);

    QMetaObject::invokeMethod(m_detectorWorker, "initialize",
                              Qt::QueuedConnection,
                              Q_ARG(QString, workdir));
}

void MainWindow::on_btnDetectorDisconnect_clicked()
{
    if (!m_detectorWorker || !m_detectorThread) {
        m_detectorReady = false;
        m_correctionUpdatePending = false;
        setAcquireState(AcquireState::Disconnected);
        updateAcquireStatusText(QStringLiteral("探测器未连接"));
        return;
    }

    stopDetectorAcquisition(true);

    QMetaObject::invokeMethod(m_detectorWorker, "shutdown",
                              Qt::BlockingQueuedConnection);

    m_detectorThread->quit();
    m_detectorThread->wait();

    m_detectorThread->deleteLater();
    m_detectorThread = nullptr;
    m_detectorWorker = nullptr;

    m_detectorReady = false;
    m_correctionUpdatePending = false;
    updateCorrectionUi(Enm_CorrectOp_Null);
    clearAcquirePreview();
    clearAcquireSession();
    setAcquireState(AcquireState::Disconnected);
    updateAcquireStatusText(QStringLiteral("探测器未连接"));
    ui->lbAcquireCorrectionStatus->setText(QStringLiteral("当前校正: 未知"));
}

void MainWindow::requestMotorSeekZero(int axisId)
{
    if (!m_motorWorker || !m_motorConnected || axisId < 0 || axisId >= kMotorAxisCount) {
        return;
    }

    const auto& axisWidgets = m_motorAxisWidgets[axisId];
    QString error;
    if (!validateMotorSeekZero(axisId,
                               axisWidgets.velocitySpin->value(),
                               axisWidgets.accelerationSpin->value(),
                               &error)) {
        QMessageBox::warning(this, QStringLiteral("Motor Safety"), error);
        return;
    }

    QMetaObject::invokeMethod(m_motorWorker, "seekZero",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(axisId)),
                              Q_ARG(float, static_cast<float>(axisWidgets.velocitySpin->value())),
                              Q_ARG(float, static_cast<float>(axisWidgets.accelerationSpin->value())));
    setMotorOperationStatusText(QStringLiteral("Axis %1 home command sent.").arg(motorAxisName(axisId)));
}

void MainWindow::requestMotorCancelSeekZero(int axisId)
{
    if (!m_motorWorker || !m_motorConnected || axisId < 0 || axisId >= kMotorAxisCount) {
        return;
    }

    QMetaObject::invokeMethod(m_motorWorker, "cancelSeekZero",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(axisId)));
    setMotorOperationStatusText(QStringLiteral("Axis %1 cancel-home command sent.").arg(motorAxisName(axisId)));
}

void MainWindow::requestMotorAbsMove(int axisId)
{
    if (!m_motorWorker || !m_motorConnected || axisId < 0 || axisId >= kMotorAxisCount) {
        return;
    }

    const auto& axisWidgets = m_motorAxisWidgets[axisId];
    QString error;
    if (!validateMotorMove(axisId,
                           false,
                           axisWidgets.absPosSpin->value(),
                           axisWidgets.velocitySpin->value(),
                           axisWidgets.accelerationSpin->value(),
                           &error)) {
        QMessageBox::warning(this, QStringLiteral("Motor Safety"), error);
        return;
    }

    QMetaObject::invokeMethod(m_motorWorker, "absMove",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(axisId)),
                              Q_ARG(float, static_cast<float>(axisWidgets.absPosSpin->value())),
                              Q_ARG(float, static_cast<float>(axisWidgets.velocitySpin->value())),
                              Q_ARG(float, static_cast<float>(axisWidgets.accelerationSpin->value())));
    setMotorOperationStatusText(QStringLiteral("Axis %1 absolute move command sent.").arg(motorAxisName(axisId)));
}

void MainWindow::requestMotorRelMove(int axisId)
{
    if (!m_motorWorker || !m_motorConnected || axisId < 0 || axisId >= kMotorAxisCount) {
        return;
    }

    const auto& axisWidgets = m_motorAxisWidgets[axisId];
    QString error;
    if (!validateMotorMove(axisId,
                           true,
                           axisWidgets.relDistSpin->value(),
                           axisWidgets.velocitySpin->value(),
                           axisWidgets.accelerationSpin->value(),
                           &error)) {
        QMessageBox::warning(this, QStringLiteral("Motor Safety"), error);
        return;
    }

    QMetaObject::invokeMethod(m_motorWorker, "relMove",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(axisId)),
                              Q_ARG(float, static_cast<float>(axisWidgets.relDistSpin->value())),
                              Q_ARG(float, static_cast<float>(axisWidgets.velocitySpin->value())),
                              Q_ARG(float, static_cast<float>(axisWidgets.accelerationSpin->value())));
    setMotorOperationStatusText(QStringLiteral("Axis %1 relative move command sent.").arg(motorAxisName(axisId)));
}

void MainWindow::requestMotorStop(int axisId)
{
    if (!m_motorWorker || !m_motorConnected || axisId < 0 || axisId >= kMotorAxisCount) {
        return;
    }

    const auto& axisWidgets = m_motorAxisWidgets[axisId];
    const auto& config = motorAxisSafetyConfig(axisId);
    if (axisWidgets.accelerationSpin->value() < kMotorMinAcceleration ||
        axisWidgets.accelerationSpin->value() > config.maxAcceleration) {
        QMessageBox::warning(
            this,
            QStringLiteral("Motor Safety"),
            QStringLiteral("Axis %1 stop deceleration must stay within [%2, %3].")
                .arg(config.label)
                .arg(QString::number(kMotorMinAcceleration, 'f', 3))
                .arg(QString::number(config.maxAcceleration, 'f', 3)));
        return;
    }

    QMetaObject::invokeMethod(m_motorWorker, "stopAxis",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(axisId)),
                              Q_ARG(float, static_cast<float>(axisWidgets.accelerationSpin->value())));
    setMotorOperationStatusText(QStringLiteral("Axis %1 stop command sent.").arg(motorAxisName(axisId)));
}

void MainWindow::requestMotorEmergencyStop(int axisId)
{
    if (!m_motorWorker || !m_motorConnected || axisId < 0 || axisId >= kMotorAxisCount) {
        return;
    }

    QMetaObject::invokeMethod(m_motorWorker, "emergencyStopAxis",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(axisId)));
    setMotorOperationStatusText(QStringLiteral("Axis %1 emergency stop command sent.").arg(motorAxisName(axisId)));
}

void MainWindow::on_btnMotorConnect_clicked()
{
    if (m_motorConnecting || m_motorConnected) {
        return;
    }

    const QString portName = ui->comboMotorPort->currentData().toString().trimmed().toUpper();
    bool numberOk = false;
    const int portNumber = portName.startsWith(QStringLiteral("COM"))
                               ? portName.mid(3).toInt(&numberOk)
                               : -1;
    if (!numberOk || portNumber <= 0 || portNumber > std::numeric_limits<quint8>::max()) {
        QMessageBox::warning(this, QStringLiteral("Notice"),
                             QStringLiteral("Please select a valid COM port before connecting."));
        return;
    }

    if (!ensureMotorWorkerThread()) {
        QMessageBox::critical(this, QStringLiteral("Motor Error"),
                              QStringLiteral("Failed to create the motor worker thread."));
        return;
    }

    m_motorConnecting = true;
    m_motorConnected = false;
    resetMotorStatusUi();
    ui->lbMotorConnectionStatusValue->setText(QStringLiteral("Connecting..."));
    setMotorOperationStatusText(QStringLiteral("Connecting to %1...").arg(portName));
    updateMotorButtons();

    QMetaObject::invokeMethod(m_motorWorker, "connectSerial",
                              Qt::QueuedConnection,
                              Q_ARG(quint8, static_cast<quint8>(portNumber)));
}

void MainWindow::on_btnMotorDisconnect_clicked()
{
    cleanupMotorThread(true);

    m_motorConnecting = false;
    m_motorConnected = false;
    resetMotorStatusUi();
    ui->lbMotorConnectionStatusValue->setText(QStringLiteral("Not connected"));
    setMotorOperationStatusText(QStringLiteral("Disconnected"));
    refreshSerialPorts();
    updateMotorButtons();
}

void MainWindow::onMotorConnectedChanged(bool connected)
{
    m_motorConnected = connected;

    if (connected) {
        ui->lbMotorConnectionStatusValue->setText(QStringLiteral("Connected"));
    } else if (!m_motorConnecting) {
        ui->lbMotorConnectionStatusValue->setText(QStringLiteral("Not connected"));
        resetMotorStatusUi();
    }

    updateMotorButtons();
}

void MainWindow::onMotorStatusUpdated(const QVector<float>& pos,
                                      const QVector<float>& actualPos,
                                      const QVector<float>& spd,
                                      const QVector<qint32>& runningStates,
                                      const QVector<quint32>& homeState)
{
    const auto floatValueAt = [](const QVector<float>& values, int index) {
        return index >= 0 && index < values.size() ? values.at(index) : 0.0f;
    };
    const auto intValueAt = [](const QVector<qint32>& values, int index) {
        return index >= 0 && index < values.size() ? values.at(index) : 0;
    };
    const auto uintValueAt = [](const QVector<quint32>& values, int index) {
        return index >= 0 && index < values.size() ? values.at(index) : 0U;
    };

    /*
     * 一次状态刷新同时落到 UI 和缓存快照里。
     * 后续所有主窗口侧的软限位/运动前校验，都依赖这份最近一次六轴快照。
     */
    m_hasMotorStatusSnapshot = true;
    for (int axisId = 0; axisId < kMotorAxisCount; ++axisId) {
        auto& axisWidgets = m_motorAxisWidgets[axisId];
        const float commandPos = floatValueAt(pos, axisId);
        const float actualPosValue = floatValueAt(actualPos, axisId);
        const qint32 running = intValueAt(runningStates, axisId);

        m_lastMotorCommandPos[axisId] = commandPos;
        m_lastMotorActualPos[axisId] = actualPosValue;
        m_lastMotorRunningStates[axisId] = running;

        axisWidgets.commandPosValue->setText(formatMotorFloat(commandPos));
        axisWidgets.actualPosValue->setText(formatMotorFloat(actualPosValue));
        axisWidgets.speedValue->setText(formatMotorFloat(floatValueAt(spd, axisId)));
        axisWidgets.runningValue->setText(motorRunningText(running));
        axisWidgets.homeValue->setText(formatMotorHomeState(uintValueAt(homeState, axisId)));
    }
}

void MainWindow::onMotorOperationFinished(const QString& opName, bool ok, const QString& message)
{
    setMotorOperationStatusText(message);

    if (opName == QStringLiteral("connectSerial") ||
        opName == QStringLiteral("connectNetwork")) {
        m_motorConnecting = false;
        if (!ok && !m_motorConnected) {
            ui->lbMotorConnectionStatusValue->setText(QStringLiteral("Connect failed"));
            resetMotorStatusUi();
        }
    }

    updateMotorButtons();
}

void MainWindow::on_cbReconAlgm_activated(int index)
{
    ui->swReconParam->setCurrentIndex(index);
}

void MainWindow::on_cbAppMode_activated(int index)
{
    if (index < 0 || m_workdirManager == nullptr)
        return;

    const int modeIndex = ui->cbAppMode->itemData(index).toInt();
    const QVector<ApplicationMode>& modes = m_workdirManager->getAppModes();

    if (modeIndex < 0 || modeIndex >= modes.size())
        return;

    m_currentMode = modes[modeIndex];
    updateAcquireFrameInfoText(-1,
                               m_currentMode.ImageWidth,
                               m_currentMode.ImageHeight,
                               kExpectedDetectorBytesPerPixel);

    if (m_detectorWorker) {
        if (m_acquireState != AcquireState::Idle &&
            m_acquireState != AcquireState::Disconnected) {
            stopDetectorAcquisition(false);
        }

        m_correctionUpdatePending = true;
        updateAcquireButtons();

        QMetaObject::invokeMethod(m_detectorWorker, "setCaliSubset",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, m_currentMode.subset));
    }

    qInfo() << "Switch detector subset to" << m_currentMode.subset
            << "| size:" << m_currentMode.ImageWidth << "x" << m_currentMode.ImageHeight
            << "| binning:" << m_currentMode.Binning
            << "| fps:" << m_currentMode.Frequency
            << "| exposure mode:" << m_currentMode.ExposureMode;
}

void MainWindow::on_btnChangeAppMode_clicked()
{
    if (workdir.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("提示"),
                              QStringLiteral("请先选择工作目录。"));
        return;
    }

    const QString iniPath = QDir(workdir).filePath("DynamicApplicationMode.ini");
    QFileInfo fileInfo(iniPath);

    if (fileInfo.exists()) {
        const bool success = QDesktopServices::openUrl(QUrl::fromLocalFile(iniPath));
        if (!success) {
            QMessageBox::critical(this, QStringLiteral("错误"),
                                  QStringLiteral("无法打开模式配置文件。"));
        }
    } else {
        QMessageBox::critical(this, QStringLiteral("错误"),
                              QStringLiteral("找不到文件:\n%1").arg(iniPath));
    }
}

bool MainWindow::parseXRayEndpoint(QString* host, quint16* port) const
{
    const QString text = ui->leXRayNetPort->text().trimmed();
    const int separator = text.lastIndexOf(':');
    if (separator <= 0 || separator >= text.size() - 1) {
        return false;
    }

    const QString hostText = text.left(separator).trimmed();
    bool ok = false;
    const uint portValue = text.mid(separator + 1).trimmed().toUInt(&ok);
    if (hostText.isEmpty() || !ok || portValue == 0 || portValue > 65535) {
        return false;
    }

    if (host) {
        *host = hostText;
    }
    if (port) {
        *port = static_cast<quint16>(portValue);
    }
    return true;
}

void MainWindow::cleanupXRayThread(bool invokeShutdown)
{
    if (!m_xrayThread) {
        m_xrayWorker = nullptr;
        return;
    }

    QThread* thread = m_xrayThread;
    XRayWorker* worker = m_xrayWorker;

    m_xrayThread = nullptr;
    m_xrayWorker = nullptr;

    if (invokeShutdown && worker) {
        QMetaObject::invokeMethod(worker, "shutdown", Qt::BlockingQueuedConnection);
    }

    thread->quit();
    thread->wait();
    thread->deleteLater();
}

void MainWindow::connectXRayWorkerSignals()
{
    /*
     * X 射线控制链的所有实际 I/O 都在 XRayWorker 线程里完成。
     * 主窗口只建立信号桥，把开关高压、设置参数和状态回传接到 UI 线程。
     */
    connect(m_xrayThread, &QThread::finished,
            m_xrayWorker, &QObject::deleteLater);

    connect(this, &MainWindow::openXRay,
            m_xrayWorker, &XRayWorker::openXRay,
            Qt::QueuedConnection);
    connect(this, &MainWindow::closeXRay,
            m_xrayWorker, &XRayWorker::closeXRay,
            Qt::QueuedConnection);
    connect(this, &MainWindow::applyXRaySettings,
            m_xrayWorker, &XRayWorker::applySettings,
            Qt::QueuedConnection);

    connect(m_xrayWorker, &XRayWorker::initialized,
            this, &MainWindow::onXRayInitialized);
    connect(m_xrayWorker, &XRayWorker::connectionChanged,
            this, &MainWindow::onXRayConnectionChanged);
    connect(m_xrayWorker, &XRayWorker::xrayStateChanged,
            this, &MainWindow::onXRayStateChanged);
    connect(m_xrayWorker, &XRayWorker::statusUpdated,
            this, &MainWindow::onXRayStatusUpdated);
    connect(m_xrayWorker, &XRayWorker::errorOccurred, this,
            [this](const QString& msg) {
                if (!msg.isEmpty()) {
                    QMessageBox::critical(this, QStringLiteral("XRay 错误"), msg);
                }
            });
}

void MainWindow::on_btnSelectXRayExePath_clicked()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("请选择 XRay 软件 exe 路径"),
        QString(),
        QStringLiteral("Executable (*.exe)"));

    if (filePath.isEmpty())
        return;

    ui->leXRayExePath->setText(filePath);
}

void MainWindow::on_btnXRayConnect_clicked()
{
    // X 光机连接同样采用“UI 线程负责建线程，worker 线程负责实际连接”的模式。
    // 这样即使厂商软件启动或 TCP 握手较慢，也不会阻塞界面。
    if (m_xrayThread || m_xrayWorker) {
        return;
    }

    QString host;
    quint16 port = 0;
    if (!parseXRayEndpoint(&host, &port)) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("光源地址格式无效，请输入 host:port，例如 127.0.0.1:3600"));
        return;
    }

    m_xrayThread = new QThread(this);
    m_xrayWorker = new XRayWorker();
    m_xrayWorker->moveToThread(m_xrayThread);
    connectXRayWorkerSignals();

    m_xrayConnecting = true;
    m_xrayConnected = false;
    m_xrayHvOn = false;
    resetXRayStatusUi();
    ui->lbXRayConnectionStatusValue->setText(QStringLiteral("连接中..."));
    updateXRayButtons();

    m_xrayThread->start();

    const QString exePath = ui->leXRayExePath->text().trimmed();
    const QString exeDir = QFileInfo(exePath).path();
    QMetaObject::invokeMethod(m_xrayWorker, "initialize",
                              Qt::QueuedConnection,
                              Q_ARG(QString, exePath),
                              Q_ARG(QStringList, QStringList{}),
                              Q_ARG(QString, exeDir),
                              Q_ARG(QString, host),
                              Q_ARG(quint16, port));
}

void MainWindow::on_btnXRayDisconnect_clicked()
{
    cleanupXRayThread(true);

    m_xrayConnecting = false;
    m_xrayConnected = false;
    m_xrayHvOn = false;
    resetXRayStatusUi();
    ui->lbXRayConnectionStatusValue->setText(QStringLiteral("未连接"));
    updateXRayButtons();
}

void MainWindow::on_btnXRayApplySettings_clicked()
{
    if (!m_xrayWorker || !m_xrayConnected) {
        return;
    }

    QString error;
    if (!validateXRaySettings(ui->dSpinXRayVoltage->value(),
                              ui->spinXRayCurrentUa->value(),
                              false,
                              &error)) {
        QMessageBox::warning(this, QStringLiteral("XRay Safety"), error);
        return;
    }

    if (m_lastXRayStatus.valid && xrayHasFault(m_lastXRayStatus)) {
        QMessageBox::warning(
            this,
            QStringLiteral("XRay Safety"),
            QStringLiteral("Cannot apply settings while the source reports a fault."));
        return;
    }

    if (m_lastXRayStatus.valid && xrayHasBlockingInterlock(m_lastXRayStatus)) {
        QMessageBox::warning(
            this,
            QStringLiteral("XRay Safety"),
            QStringLiteral("Cannot apply settings while an interlock is active: %1")
                .arg(xrayInterlockText(m_lastXRayStatus.faceBitData)));
        return;
    }

    emit applyXRaySettings(ui->dSpinXRayVoltage->value(),
                           ui->spinXRayCurrentUa->value());
}

void MainWindow::on_btnXRayOpen_clicked()
{
    if (!m_xrayWorker || !m_xrayConnected) {
        return;
    }

    QString error;
    if (!canOpenXRay(&error)) {
        QMessageBox::warning(this, QStringLiteral("XRay Safety"), error);
        return;
    }

    emit openXRay();
}

void MainWindow::on_btnXRayClose_clicked()
{
    if (!m_xrayWorker || !m_xrayConnected) {
        return;
    }

    emit closeXRay();
}

void MainWindow::onXRayInitialized(bool ok)
{
    m_xrayConnecting = false;

    if (!ok) {
        m_xrayConnected = false;
        m_xrayHvOn = false;
        resetXRayStatusUi();
        ui->lbXRayConnectionStatusValue->setText(QStringLiteral("连接失败"));
        cleanupXRayThread(false);
    }

    updateXRayButtons();
}

void MainWindow::onXRayConnectionChanged(bool connected)
{
    m_xrayConnected = connected;

    if (connected) {
        ui->lbXRayConnectionStatusValue->setText(QStringLiteral("已连接（厂商软件就绪）"));
    } else {
        m_xrayHvOn = false;
        resetXRayStatusUi();
        ui->lbXRayConnectionStatusValue->setText(QStringLiteral("未连接"));

        if (m_xrayThread && !m_xrayConnecting) {
            cleanupXRayThread(false);
        }
    }

    updateXRayButtons();
}

void MainWindow::onXRayStateChanged(bool hvOn)
{
    m_xrayHvOn = hvOn;

    if (!m_lastXRayStatus.valid) {
        ui->lbXRayHvStatusValue->setText(hvOn
            ? QStringLiteral("高压开启")
            : QStringLiteral("高压关闭"));
    }

    updateXRayButtons();
}

void MainWindow::onXRayStatusUpdated(const XRayController::Status& status)
{
    updateXRayStatusUi(status);
    m_xrayHvOn = status.valid && status.faceState == kXRayHvOnFaceState;
    updateXRayButtons();
}

void MainWindow::on_btnSelectAcquireSaveDir_clicked()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择采集保存目录"),
        ui->leAcquireSaveDir->text());
    if (dir.isEmpty())
        return;

    ui->leAcquireSaveDir->setText(dir);
}

void MainWindow::on_btnAcquireSingle_clicked()
{
    startDetectorAcquisition(false);
}

void MainWindow::on_btnAcquireStart_clicked()
{
    startDetectorAcquisition(true);
}

void MainWindow::on_btnAcquireStop_clicked()
{
    stopDetectorAcquisition(false);
}

void MainWindow::on_cbCorrectionOffset_toggled(bool checked)
{
    Q_UNUSED(checked);
    applyCorrectionSelection();
}

void MainWindow::on_cbCorrectionGain_toggled(bool checked)
{
    Q_UNUSED(checked);
    applyCorrectionSelection();
}

void MainWindow::on_cbCorrectionDefect_toggled(bool checked)
{
    Q_UNUSED(checked);
    applyCorrectionSelection();
}

void MainWindow::onDetectorInitialized(bool ok)
{
    m_detectorReady = ok;
    if (!ok) {
        setAcquireState(AcquireState::Disconnected);
        updateAcquireStatusText(QStringLiteral("探测器连接失败"));
        return;
    }

    setAcquireState(AcquireState::Idle);
    updateAcquireStatusText(QStringLiteral("探测器已连接"));

    if (ui->cbAppMode->count() > 0 && ui->cbAppMode->currentIndex() >= 0)
        on_cbAppMode_activated(ui->cbAppMode->currentIndex());
    else
        requestCurrentCorrectOption();
}

void MainWindow::onDetectorCurrentCorrectOptionRead(int option)
{
    updateCorrectionUi(option);
}

void MainWindow::onDetectorCurrentCorrectOptionApplied(int option)
{
    updateCorrectionUi(option);
}

void MainWindow::onDetectorAcquisitionStateChanged(bool running, bool continuous)
{
    const AcquireState previousState = m_acquireState;

    if (running) {
        setAcquireState(continuous ? AcquireState::Continuous : AcquireState::Single);
        updateAcquireStatusText(continuous
            ? QStringLiteral("连续采集中...")
            : QStringLiteral("单帧采集中..."));
        return;
    }

    setAcquireState(m_detectorReady ? AcquireState::Idle : AcquireState::Disconnected);

    if (previousState == AcquireState::Single ||
        previousState == AcquireState::StartingSingle) {
        updateAcquireStatusText(QStringLiteral("单帧采集完成"));
    } else if (previousState == AcquireState::Continuous ||
               previousState == AcquireState::StartingContinuous ||
               previousState == AcquireState::Stopping) {
        updateAcquireStatusText(QStringLiteral("采集已停止"));
    } else if (m_detectorReady) {
        updateAcquireStatusText(QStringLiteral("探测器已连接"));
    } else {
        updateAcquireStatusText(QStringLiteral("探测器未连接"));
    }
}

void MainWindow::onDetectorImageReady(const QByteArray& image,
                                      int imageSize,
                                      int frameNo,
                                      int width,
                                      int height,
                                      int bytesPerPixel)
{
    Q_UNUSED(imageSize);

    updateAcquireFrameInfoText(frameNo, width, height, bytesPerPixel);

    m_lastPreviewImage = buildPreviewImage(image, width, height, bytesPerPixel);
    updatePreviewPixmap();

    if (!m_acquireSessionDir.isEmpty()) {
        emit saveAcquiredFrame(m_acquireSessionDir,
                               m_nextAcquireSequenceNo++,
                               image,
                               width,
                               height,
                               bytesPerPixel);
    }
}

void MainWindow::onAcquireFrameSaved(int sequenceNo, const QString& filePath)
{
    Q_UNUSED(sequenceNo);
    Q_UNUSED(filePath);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updatePreviewPixmap();
}

ReconWorker::Task MainWindow::buildReconstructionTask() const
{
    ReconWorker::Task task;

    switch (ui->cbReconAlgm->currentIndex()) {
    case 1:
        task.algorithm = ReconWorker::Algorithm::SART;
        break;
    case 0:
    default:
        task.algorithm = ReconWorker::Algorithm::FDK;
        break;
    }

    task.useCuda = ui->rbCUDA->isChecked();
    task.projectionDirectory = ui->leProjFolder->text();
    task.outputFilePath = ui->leReconSavePath->text();

    task.projectionImageParams.spacing[0] = ui->dSpinDetSpacingX->value();
    task.projectionImageParams.spacing[1] = ui->dSpinDetSpacingY->value();
    task.projectionImageParams.spacing[2] = 1.0;
    task.projectionImageParams.origin[0] =
        -(ui->spinDetU->value() - 1) * task.projectionImageParams.spacing[0] / 2;
    task.projectionImageParams.origin[1] =
        -(ui->spinDetV->value() - 1) * task.projectionImageParams.spacing[1] / 2;
    task.projectionImageParams.origin[2] = 0.0;

    task.volumeParams.spacing[0] = ui->dSpinVolSpacingX->value();
    task.volumeParams.spacing[1] = ui->dSpinVolSpacingZ->value();
    task.volumeParams.spacing[2] = ui->dSpinVolSpacingY->value();
    task.volumeParams.size[0] = ui->spinVolX->value();
    task.volumeParams.size[1] = ui->spinVolZ->value();
    task.volumeParams.size[2] = ui->spinVolY->value();
    task.volumeParams.origin[0] =
        -(task.volumeParams.size[0] - 1.0) * task.volumeParams.spacing[0] / 2;
    task.volumeParams.origin[1] =
        -(task.volumeParams.size[1] - 1.0) * task.volumeParams.spacing[1] / 2;
    task.volumeParams.origin[2] =
        -(task.volumeParams.size[2] - 1.0) * task.volumeParams.spacing[2] / 2;

    const int projectionCount = ui->spinAngleNum->value();
    const double sid = ui->dSpinSID->value();
    const double sdd = ui->dSpinSDD->value();
    task.geometryParams.views.clear();
    task.geometryParams.views.reserve(projectionCount);
    for (int projectionIndex = 0; projectionIndex < projectionCount; ++projectionIndex) {
        FdkPipeline::ProjectionViewParam view;
        view.sid = sid;
        view.sdd = sdd;
        view.gantryAngleDeg = static_cast<double>(projectionIndex) / projectionCount * 360.0;
        view.projOffsetX = ui->dSpinOffsetX->value();
        view.projOffsetY = ui->dSpinOffsetY->value();
        task.geometryParams.views.push_back(view);
    }

    task.fdkParams.hannCutFrequency = ui->dSpinFDKhannCut->value();
    task.fdkParams.truncationCorrection = ui->dSpinFDKtrucCorrect->value();
    task.fdkParams.applyFieldOfViewMask = ui->cbFDKFoV->isChecked();

    task.initParams.mode = task.useCuda
        ? SartPipeline::InitializationMode::FDK_CUDA
        : SartPipeline::InitializationMode::FDK_CPU;
    task.initParams.fdkParams = task.fdkParams;

    task.sartParams.numberOfIterations = static_cast<unsigned int>(ui->spinSARTIter->value());
    task.sartParams.numberOfProjectionsPerSubset = ui->spinSARTSubset->value();
    task.sartParams.lambda = ui->dSpinSARTLambda->value();
    task.sartParams.applyFieldOfViewMask = ui->cbSARTFoV->isChecked();

    task.tvParams.enabled = ui->spinSARTTVIter->value() > 0;
    task.tvParams.numberOfIterations = static_cast<unsigned int>(ui->spinSARTTVIter->value());
    task.tvParams.gamma = ui->dSpinSARTTVGamma->value();
    return task;
}

void MainWindow::startReconstructionTask(const ReconWorker::Task& task)
{
    /*
     * MainWindow 只负责把 UI 参数打包后交给 ReconWorker。
     * 真正的重建执行、进度上报和取消检查都在独立线程里完成。
     */
    ui->btnReconEx->setEnabled(false);
    ui->btnReconCancel->setEnabled(true);
    ui->lbReconProgress->setText(QStringLiteral("正在启动重建任务..."));

    m_reconThread = new QThread(this);
    m_reconWorker = new ReconWorker(task);
    m_reconWorker->moveToThread(m_reconThread);

    connect(m_reconThread, &QThread::started,
            m_reconWorker, &ReconWorker::process);

    connect(m_reconWorker, &ReconWorker::progress,
            this, [this](const QString& msg) {
                ui->lbReconProgress->setText(msg);
            });

    connect(m_reconWorker, &ReconWorker::finished,
            this, [this](const QString& path) {
                Q_UNUSED(path);
                ui->lbReconProgress->setText(QStringLiteral("重建完成"));
            });

    connect(m_reconWorker, &ReconWorker::failed,
            this, [this](const QString& err) {
                QMessageBox::critical(this, QStringLiteral("重建错误"), err);
                qCritical() << err;
            });

    connect(m_reconWorker, &ReconWorker::canceled,
            this, [this](const QString& msg) {
                ui->lbReconProgress->setText(msg);
            });

    connect(m_reconWorker, &ReconWorker::finished,
            m_reconThread, &QThread::quit);
    connect(m_reconWorker, &ReconWorker::failed,
            m_reconThread, &QThread::quit);
    connect(m_reconWorker, &ReconWorker::canceled,
            m_reconThread, &QThread::quit);

    connect(m_reconThread, &QThread::finished,
            m_reconWorker, &QObject::deleteLater);
    connect(m_reconThread, &QThread::finished,
            m_reconThread, &QObject::deleteLater);
    connect(m_reconThread, &QThread::finished,
            this, [this]() {
                ui->btnReconEx->setEnabled(true);
                ui->btnReconCancel->setEnabled(false);
                m_reconWorker = nullptr;
                m_reconThread = nullptr;
            });

    m_reconThread->start();
}

void MainWindow::on_btnReconEx_clicked()
{
    if (m_reconThread)
        return;

    QString error;
    if (!validateReconstructionInputs(&error)) {
        QMessageBox::warning(this, QStringLiteral("Reconstruction"), error);
        return;
    }

    startReconstructionTask(buildReconstructionTask());
}

void MainWindow::on_btnReconCancel_clicked()
{
    if (!m_reconWorker)
        return;

    ui->btnReconCancel->setEnabled(false);
    m_reconWorker->requestStop();
    ui->lbReconProgress->setText(QStringLiteral("已发送停止请求，等待当前计算阶段结束..."));
}

void MainWindow::on_cbFDKFoV_checkStateChanged(const Qt::CheckState &state)
{
    ui->cbSARTFoV->setCheckState(state);
}

void MainWindow::on_cbSARTFoV_checkStateChanged(const Qt::CheckState &state)
{
    ui->cbFDKFoV->setCheckState(state);
}

void MainWindow::on_btnSelectProjFolder_clicked()
{
    const QString projDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择投影文件目录"), QString());
    if (projDir.isEmpty())
        return;

    ui->leProjFolder->setText(projDir);
}

void MainWindow::on_btSelectReconSavePath_clicked()
{
    QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("请选择保存文件路径"),
        QString(),
        QStringLiteral("(*.mha)"));

    if(filePath.isEmpty())
        return;

    if (!filePath.endsWith(".mha", Qt::CaseInsensitive))
        filePath += ".mha";

    ui->leReconSavePath->setText(filePath);
}
