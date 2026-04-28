#include "imageviewer.h"

#include "mprsliceview.h"
#include "mprvolumeview.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMetaImageReader.h>
#include <vtkMetaImageWriter.h>
#include <vtkNIFTIImageReader.h>
#include <vtkNIFTIImageWriter.h>
#include <vtkPointData.h>

namespace
{
constexpr int kSampleValueLimit = 200000;
constexpr double kAutoWindowLowPercentile = 0.02;
constexpr double kAutoWindowHighPercentile = 0.99;
constexpr int kCtSoftTissueWindowWidth = 400;
constexpr int kCtSoftTissueWindowLevel = 40;

QString convertToShortPath(const QString& longPath)
{
    wchar_t shortPathBuffer[MAX_PATH] = {0};
    const DWORD result = GetShortPathNameW(reinterpret_cast<LPCWSTR>(longPath.utf16()),
                                           shortPathBuffer,
                                           MAX_PATH);
    if (result == 0 || result >= MAX_PATH) {
        return longPath;
    }
    return QString::fromWCharArray(shortPathBuffer);
}

QString decompressGzFile(const QString& gzFilePath)
{
    const QFileInfo gzFileInfo(gzFilePath);
    if (!gzFileInfo.exists() || !gzFileInfo.isReadable()) {
        return {};
    }

    const QString tempDir = QDir::tempPath() + "/HNUCT_nii_temp";
    if (!QDir().mkpath(tempDir)) {
        return {};
    }

    QString outputFilePath = QDir(tempDir).filePath(gzFileInfo.completeBaseName());
    if (!outputFilePath.endsWith(".nii", Qt::CaseInsensitive)) {
        outputFilePath += ".nii";
    }
    QFile::remove(outputFilePath);

    QString inputEscaped = gzFilePath;
    QString outputEscaped = outputFilePath;
    inputEscaped.replace('\'', "''");
    outputEscaped.replace('\'', "''");

    const QString command = QString(
        "& { "
        "$inputFile='%1'; "
        "$outputFile='%2'; "
        "$input=[System.IO.File]::OpenRead($inputFile); "
        "$gzip=New-Object System.IO.Compression.GzipStream($input,[System.IO.Compression.CompressionMode]::Decompress); "
        "$output=[System.IO.File]::Create($outputFile); "
        "$gzip.CopyTo($output); "
        "$output.Close(); "
        "$gzip.Close(); "
        "$input.Close(); "
        "}")
        .arg(inputEscaped, outputEscaped);

    QProcess process;
    process.start("powershell.exe", {"-ExecutionPolicy", "Bypass", "-Command", command});
    if (!process.waitForStarted(2000)) {
        return {};
    }
    if (!process.waitForFinished(30000) || process.exitCode() != 0) {
        QFile::remove(outputFilePath);
        return {};
    }

    return QFile::exists(outputFilePath) ? outputFilePath : QString();
}

std::vector<double> collectSampleValues(vtkImageData* imageData)
{
    std::vector<double> values;
    if (!imageData || !imageData->GetPointData()) {
        return values;
    }

    vtkDataArray* scalars = imageData->GetPointData()->GetScalars();
    if (!scalars) {
        return values;
    }

    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    if (tupleCount <= 0) {
        return values;
    }

    const vtkIdType step = qMax<vtkIdType>(1, tupleCount / kSampleValueLimit);
    values.reserve(static_cast<std::size_t>((tupleCount + step - 1) / step));

    for (vtkIdType index = 0; index < tupleCount; index += step) {
        const double value = scalars->GetComponent(index, 0);
        if (std::isfinite(value)) {
            values.push_back(value);
        }
    }

    std::sort(values.begin(), values.end());
    return values;
}

double percentileValue(const std::vector<double>& sortedValues, double percentile)
{
    if (sortedValues.empty()) {
        return 0.0;
    }

    const double clampedPercentile = std::clamp(percentile, 0.0, 1.0);
    const double scaledIndex = clampedPercentile * static_cast<double>(sortedValues.size() - 1);
    const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(scaledIndex));
    const std::size_t upperIndex = static_cast<std::size_t>(std::ceil(scaledIndex));
    const double fraction = scaledIndex - static_cast<double>(lowerIndex);

    return sortedValues[lowerIndex] +
           (sortedValues[upperIndex] - sortedValues[lowerIndex]) * fraction;
}

bool looksLikeCtHuRange(double minimum, double maximum)
{
    return minimum < -700.0 && maximum > 700.0;
}

void chooseInitialWindow(vtkImageData* imageData,
                         double scalarMinimum,
                         double scalarMaximum,
                         int* windowLevelOut,
                         int* windowWidthOut)
{
    int windowLevel = qRound((scalarMinimum + scalarMaximum) * 0.5);
    int windowWidth = qMax(1, qRound(scalarMaximum - scalarMinimum));

    const auto sampledValues = collectSampleValues(imageData);

    if (looksLikeCtHuRange(scalarMinimum, scalarMaximum)) {
        windowLevel = kCtSoftTissueWindowLevel;
        windowWidth = kCtSoftTissueWindowWidth;
    } else if (!sampledValues.empty()) {
        const double low = percentileValue(sampledValues, kAutoWindowLowPercentile);
        const double high = percentileValue(sampledValues, kAutoWindowHighPercentile);
        if (high > low) {
            windowLevel = qRound((low + high) * 0.5);
            windowWidth = qMax(1, qRound(high - low));
        }
    }

    const int roundedMinimum = static_cast<int>(std::floor(scalarMinimum));
    const int roundedMaximum = static_cast<int>(std::ceil(scalarMaximum));
    windowLevel = qBound(roundedMinimum, windowLevel, roundedMaximum);
    windowWidth = qBound(1, windowWidth, qMax(1, roundedMaximum - roundedMinimum));

    if (windowLevelOut) {
        *windowLevelOut = windowLevel;
    }
    if (windowWidthOut) {
        *windowWidthOut = windowWidth;
    }
}
}

ImageViewer::ImageViewer(QWidget* parent)
    : QWidget(parent)
{
    setupViewer();

    m_labelRefreshTimer = new QTimer(this);
    m_labelRefreshTimer->setSingleShot(true);
    m_labelRefreshTimer->setInterval(90);
    connect(m_labelRefreshTimer, &QTimer::timeout,
            this, &ImageViewer::flushPendingLabelUpdate);
}

bool ImageViewer::loadImage(const QString& fileName)
{
    const QFileInfo fileInfo(fileName);
    if (!fileInfo.exists()) {
        return false;
    }

    const bool isNiiGz = fileName.endsWith(".nii.gz", Qt::CaseInsensitive);
    QString extension = fileInfo.suffix().toLower();
    QString actualFilePath = fileName;
    bool removeTempFile = false;

    if (isNiiGz || extension == "gz") {
        actualFilePath = decompressGzFile(fileName);
        if (actualFilePath.isEmpty()) {
            return false;
        }
        extension = "nii";
        removeTempFile = true;
    }

    vtkImageData* loadedImageData = nullptr;

    if (extension == "mhd" || extension == "mha") {
        if (!m_mhdReader) {
            m_mhdReader = vtkSmartPointer<vtkMetaImageReader>::New();
        }
        const QString shortPath = convertToShortPath(actualFilePath);
        m_mhdReader->SetFileName(shortPath.toLocal8Bit().constData());
        m_mhdReader->Update();
        loadedImageData = m_mhdReader->GetOutput();
    } else if (extension == "nii") {
        if (!m_niiReader) {
            m_niiReader = vtkSmartPointer<vtkNIFTIImageReader>::New();
        }
        const QString shortPath = convertToShortPath(actualFilePath);
        m_niiReader->SetFileName(shortPath.toLocal8Bit().constData());
        m_niiReader->Update();
        loadedImageData = m_niiReader->GetOutput();
    } else {
        if (removeTempFile) {
            QFile::remove(actualFilePath);
        }
        return false;
    }

    bool success = false;
    if (loadedImageData && loadedImageData->GetNumberOfPoints() > 0) {
        auto ownedImage = vtkSmartPointer<vtkImageData>::New();
        ownedImage->DeepCopy(loadedImageData);
        success = adoptVolume(ownedImage, false);
    }

    if (removeTempFile) {
        QFile::remove(actualFilePath);
    }

    return success;
}

bool ImageViewer::saveImage(const QString& fileName)
{
    if (!m_imageData) {
        return false;
    }

    const QString lowerFileName = fileName.toLower();
    if (lowerFileName.endsWith(".png") ||
        lowerFileName.endsWith(".jpg") ||
        lowerFileName.endsWith(".jpeg") ||
        lowerFileName.endsWith(".bmp")) {
        const QImage snapshot = currentSliceSnapshot();
        return !snapshot.isNull() && snapshot.save(fileName);
    }

    if (lowerFileName.endsWith(".mhd") || lowerFileName.endsWith(".mha")) {
        auto writer = vtkSmartPointer<vtkMetaImageWriter>::New();
        writer->SetFileName(fileName.toStdString().c_str());
        writer->SetInputData(m_imageData);
        writer->Write();
        return true;
    }

    if (lowerFileName.endsWith(".nii")) {
        auto writer = vtkSmartPointer<vtkNIFTIImageWriter>::New();
        writer->SetFileName(fileName.toStdString().c_str());
        writer->SetInputData(m_imageData);
        writer->Write();
        return true;
    }

    return false;
}

void ImageViewer::setWindowLevel(int level)
{
    applyWindowLevelInternal(level, m_windowWidth, true);
}

void ImageViewer::setWindowWidth(int width)
{
    applyWindowLevelInternal(m_windowLevel, width, true);
}

void ImageViewer::setWindowLevelWidth(int level, int width)
{
    applyWindowLevelInternal(level, width, true);
}

void ImageViewer::setBackgroundColor(const QString& color)
{
    const QColor parsedColor(color);
    m_backgroundColor = parsedColor.isValid() ? parsedColor : QColor(Qt::black);
    applyBackgroundColor();
}

void ImageViewer::setInterfaceTheme(bool darkTheme)
{
    if (m_darkTheme == darkTheme) {
        return;
    }

    m_darkTheme = darkTheme;
    applyToolbarTheme();
}

void ImageViewer::loadTestImage()
{
    auto testImage = vtkSmartPointer<vtkImageData>::New();
    testImage->SetDimensions(128, 128, 96);
    testImage->SetSpacing(1.0, 1.0, 1.0);
    testImage->SetOrigin(0.0, 0.0, 0.0);
    testImage->AllocateScalars(VTK_SHORT, 1);

    const int* dims = testImage->GetDimensions();
    const double cx = static_cast<double>(dims[0] - 1) * 0.5;
    const double cy = static_cast<double>(dims[1] - 1) * 0.5;
    const double cz = static_cast<double>(dims[2] - 1) * 0.5;

    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                const double dx = (x - cx) / 42.0;
                const double dy = (y - cy) / 36.0;
                const double dz = (z - cz) / 28.0;
                const double body = dx * dx + dy * dy + dz * dz;

                short value = static_cast<short>(-1000);
                if (body < 1.0) {
                    value = static_cast<short>(40.0 + (1.0 - body) * 220.0 + std::sin(z * 0.12) * 40.0);
                }

                const double sphereDx = x - (cx + 16.0);
                const double sphereDy = y - (cy - 10.0);
                const double sphereDz = z - cz;
                if (sphereDx * sphereDx + sphereDy * sphereDy + sphereDz * sphereDz < 11.0 * 11.0) {
                    value = 900;
                }

                auto* pixel = static_cast<short*>(testImage->GetScalarPointer(x, y, z));
                if (pixel) {
                    *pixel = value;
                }
            }
        }
    }

    adoptVolume(testImage, true);
}

void ImageViewer::setZoomLevel(int level)
{
    m_zoomLevel = qMax(10, level);
    applySlicePresentation();
    emit viewStateChanged();
}

int ImageViewer::getZoomLevel() const
{
    return m_zoomLevel;
}

void ImageViewer::setCurrentSlice(int slice)
{
    if (!m_imageData) {
        return;
    }

    std::array<int, 3> nextCrosshair = m_crosshair;
    switch (m_currentOrientation) {
    case SliceOrientation::Axial:
        nextCrosshair[2] = slice;
        break;
    case SliceOrientation::Coronal:
        nextCrosshair[1] = slice;
        break;
    case SliceOrientation::Sagittal:
        nextCrosshair[0] = slice;
        break;
    }

    updateCrosshair(nextCrosshair, true);
}

int ImageViewer::getCurrentSlice() const
{
    switch (m_currentOrientation) {
    case SliceOrientation::Axial:
        return m_crosshair[2];
    case SliceOrientation::Coronal:
        return m_crosshair[1];
    case SliceOrientation::Sagittal:
        return m_crosshair[0];
    }

    return 0;
}

int ImageViewer::getWindowLevel() const
{
    return m_windowLevel;
}

int ImageViewer::getWindowWidth() const
{
    return m_windowWidth;
}

int ImageViewer::getTotalSlices() const
{
    if (!m_imageData) {
        return 0;
    }

    const int* dims = m_imageData->GetDimensions();
    switch (m_currentOrientation) {
    case SliceOrientation::Axial:
        return dims[2];
    case SliceOrientation::Coronal:
        return dims[1];
    case SliceOrientation::Sagittal:
        return dims[0];
    }

    return 0;
}

double ImageViewer::getScalarMinimum() const
{
    return m_scalarMinimum;
}

double ImageViewer::getScalarMaximum() const
{
    return m_scalarMaximum;
}

vtkSmartPointer<vtkImageData> ImageViewer::getImageData() const
{
    return m_imageData;
}

SliceOrientation ImageViewer::getCurrentOrientation() const
{
    return m_currentOrientation;
}

void ImageViewer::onSliceActivated(SliceOrientation orientation)
{
    setActiveOrientation(orientation, true);
}

void ImageViewer::onSliceCrosshairChanged(const std::array<int, 3>& crosshair)
{
    updateCrosshair(crosshair, true);
}

void ImageViewer::onSliceLabelModified()
{
    applySlicePresentation();
    scheduleVolumeRebuild();
}

void ImageViewer::onBrushModeToggled(bool checked)
{
    m_brushEditingEnabled = checked;

    if (!checked && m_eraseCheck && m_eraseCheck->isChecked()) {
        const QSignalBlocker blocker(m_eraseCheck);
        m_eraseCheck->setChecked(false);
        m_eraseMode = false;
    }

    if (m_eraseCheck) {
        m_eraseCheck->setEnabled(checked);
    }

    applySlicePresentation();
}

void ImageViewer::onEraseModeToggled(bool checked)
{
    m_eraseMode = checked && m_brushEditingEnabled;
    applySlicePresentation();
}

void ImageViewer::onLabelVisibleToggled(bool checked)
{
    m_labelVisible = checked;
    applySlicePresentation();
    applyVolumePresentation();
}

void ImageViewer::onLabelOpacityChanged(int value)
{
    m_labelOpacity = qBound(0.0, static_cast<double>(value) / 100.0, 1.0);
    applySlicePresentation();
    applyVolumePresentation();
}

void ImageViewer::onBrushRadiusChanged(int value)
{
    m_brushRadius = qMax(1, value);
    applySlicePresentation();
}

void ImageViewer::onClearLabelClicked()
{
    if (!m_labelData || !m_labelData->GetPointData() || !m_labelData->GetPointData()->GetScalars()) {
        return;
    }

    m_labelData->GetPointData()->GetScalars()->FillComponent(0, 0.0);
    m_labelData->GetPointData()->GetScalars()->Modified();
    m_labelData->Modified();
    applySlicePresentation();
    rebuildVolumeSurface();
}

void ImageViewer::flushPendingLabelUpdate()
{
    rebuildVolumeSurface();
}

void ImageViewer::setupViewer()
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(0, 0);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(6);

    setupToolbar();
    rootLayout->addWidget(m_toolbarContainer);

    setupViewGrid();
    rootLayout->addWidget(m_viewGridContainer, 1);
}

void ImageViewer::setupToolbar()
{
    m_toolbarContainer = new QWidget(this);
    m_toolbarContainer->setObjectName(QStringLiteral("toolbarContainer"));

    auto* toolbarLayout = new QHBoxLayout(m_toolbarContainer);
    toolbarLayout->setContentsMargins(10, 6, 10, 6);
    toolbarLayout->setSpacing(10);

    auto* titleLabel = new QLabel(QStringLiteral("MPR / Label"), m_toolbarContainer);
    titleLabel->setStyleSheet("font-weight: 600;");

    m_showLabelCheck = new QCheckBox(QStringLiteral("显示标签"), m_toolbarContainer);
    m_showLabelCheck->setChecked(m_labelVisible);

    m_brushCheck = new QCheckBox(QStringLiteral("画笔"), m_toolbarContainer);
    m_brushCheck->setChecked(m_brushEditingEnabled);

    m_eraseCheck = new QCheckBox(QStringLiteral("擦除"), m_toolbarContainer);
    m_eraseCheck->setChecked(m_eraseMode);
    m_eraseCheck->setEnabled(false);

    auto* opacityLabel = new QLabel(QStringLiteral("透明度"), m_toolbarContainer);
    m_labelOpacitySlider = new QSlider(Qt::Horizontal, m_toolbarContainer);
    m_labelOpacitySlider->setRange(0, 100);
    m_labelOpacitySlider->setValue(qRound(m_labelOpacity * 100.0));
    m_labelOpacitySlider->setFixedWidth(120);

    auto* radiusLabel = new QLabel(QStringLiteral("笔刷"), m_toolbarContainer);
    m_brushRadiusSpin = new QSpinBox(m_toolbarContainer);
    m_brushRadiusSpin->setRange(1, 64);
    m_brushRadiusSpin->setValue(m_brushRadius);
    m_brushRadiusSpin->setSuffix(QStringLiteral(" px"));

    m_clearLabelButton = new QPushButton(QStringLiteral("清空标签"), m_toolbarContainer);

    toolbarLayout->addWidget(titleLabel);
    toolbarLayout->addSpacing(8);
    toolbarLayout->addWidget(m_showLabelCheck);
    toolbarLayout->addWidget(m_brushCheck);
    toolbarLayout->addWidget(m_eraseCheck);
    toolbarLayout->addSpacing(8);
    toolbarLayout->addWidget(opacityLabel);
    toolbarLayout->addWidget(m_labelOpacitySlider);
    toolbarLayout->addWidget(radiusLabel);
    toolbarLayout->addWidget(m_brushRadiusSpin);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(m_clearLabelButton);

    connect(m_showLabelCheck, &QCheckBox::toggled,
            this, &ImageViewer::onLabelVisibleToggled);
    connect(m_brushCheck, &QCheckBox::toggled,
            this, &ImageViewer::onBrushModeToggled);
    connect(m_eraseCheck, &QCheckBox::toggled,
            this, &ImageViewer::onEraseModeToggled);
    connect(m_labelOpacitySlider, &QSlider::valueChanged,
            this, &ImageViewer::onLabelOpacityChanged);
    connect(m_brushRadiusSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &ImageViewer::onBrushRadiusChanged);
    connect(m_clearLabelButton, &QPushButton::clicked,
            this, &ImageViewer::onClearLabelClicked);

    applyToolbarTheme();
}

void ImageViewer::setupViewGrid()
{
    m_viewGridContainer = new QWidget(this);
    m_viewGridContainer->setObjectName(QStringLiteral("viewGridContainer"));

    auto* gridLayout = new QGridLayout(m_viewGridContainer);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setHorizontalSpacing(6);
    gridLayout->setVerticalSpacing(6);

    m_axialView = new MprSliceView(SliceOrientation::Axial, m_viewGridContainer);
    m_coronalView = new MprSliceView(SliceOrientation::Coronal, m_viewGridContainer);
    m_sagittalView = new MprSliceView(SliceOrientation::Sagittal, m_viewGridContainer);
    m_volumeView = new MprVolumeView(m_viewGridContainer);

    gridLayout->addWidget(m_axialView, 0, 0);
    gridLayout->addWidget(m_coronalView, 0, 1);
    gridLayout->addWidget(m_sagittalView, 1, 0);
    gridLayout->addWidget(m_volumeView, 1, 1);
    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setRowStretch(0, 1);
    gridLayout->setRowStretch(1, 1);

    const auto connectSliceView = [this](MprSliceView* view) {
        connect(view, &MprSliceView::activated,
                this, &ImageViewer::onSliceActivated);
        connect(view, &MprSliceView::crosshairChanged,
                this, &ImageViewer::onSliceCrosshairChanged);
        connect(view, &MprSliceView::labelModified,
                this, &ImageViewer::onSliceLabelModified);
    };

    connectSliceView(m_axialView);
    connectSliceView(m_coronalView);
    connectSliceView(m_sagittalView);

    applyBackgroundColor();
    applySlicePresentation();
    applyVolumePresentation();
}

void ImageViewer::applyToolbarTheme()
{
    if (!m_toolbarContainer) {
        return;
    }

    const QString toolbarStyle = m_darkTheme
        ? QStringLiteral(
              "QWidget#toolbarContainer {"
              "background-color: #0f172a;"
              "border: 1px solid #1e293b;"
              "border-radius: 10px;"
              "}"
              "QWidget#toolbarContainer QLabel,"
              "QWidget#toolbarContainer QCheckBox {"
              "color: #e2e8f0;"
              "background: transparent;"
              "}"
              "QWidget#toolbarContainer QPushButton,"
              "QWidget#toolbarContainer QSpinBox {"
              "background-color: #111827;"
              "color: #f8fafc;"
              "border: 1px solid #334155;"
              "border-radius: 6px;"
              "padding: 4px 8px;"
              "}"
              "QWidget#toolbarContainer QPushButton:hover {"
              "background-color: #1e293b;"
              "}"
              "QWidget#toolbarContainer QSlider::groove:horizontal {"
              "height: 6px;"
              "background: #1e293b;"
              "border-radius: 3px;"
              "}"
              "QWidget#toolbarContainer QSlider::sub-page:horizontal {"
              "background: #0ea5e9;"
              "border-radius: 3px;"
              "}"
              "QWidget#toolbarContainer QSlider::handle:horizontal {"
              "width: 14px;"
              "margin: -5px 0;"
              "border-radius: 7px;"
              "background: #f8fafc;"
              "border: 1px solid #38bdf8;"
              "}")
        : QStringLiteral(
              "QWidget#toolbarContainer {"
              "background-color: #ffffff;"
              "border: 1px solid #d6deea;"
              "border-radius: 10px;"
              "}"
              "QWidget#toolbarContainer QLabel,"
              "QWidget#toolbarContainer QCheckBox {"
              "color: #0f172a;"
              "background: transparent;"
              "}"
              "QWidget#toolbarContainer QPushButton,"
              "QWidget#toolbarContainer QSpinBox {"
              "background-color: #f8fafc;"
              "color: #0f172a;"
              "border: 1px solid #cbd5e1;"
              "border-radius: 6px;"
              "padding: 4px 8px;"
              "}"
              "QWidget#toolbarContainer QPushButton:hover {"
              "background-color: #eef2ff;"
              "}"
              "QWidget#toolbarContainer QSlider::groove:horizontal {"
              "height: 6px;"
              "background: #dbe4f0;"
              "border-radius: 3px;"
              "}"
              "QWidget#toolbarContainer QSlider::sub-page:horizontal {"
              "background: #2563eb;"
              "border-radius: 3px;"
              "}"
              "QWidget#toolbarContainer QSlider::handle:horizontal {"
              "width: 14px;"
              "margin: -5px 0;"
              "border-radius: 7px;"
              "background: #ffffff;"
              "border: 1px solid #2563eb;"
              "}");

    m_toolbarContainer->setStyleSheet(toolbarStyle);
}

bool ImageViewer::adoptVolume(vtkSmartPointer<vtkImageData> imageData, bool seedDemoLabel)
{
    if (!imageData || imageData->GetNumberOfPoints() == 0) {
        return false;
    }

    const int* dims = imageData->GetDimensions();
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }

    m_imageData = imageData;
    m_zoomLevel = 100;
    m_currentOrientation = SliceOrientation::Axial;
    m_crosshair = {{
        qMax(0, dims[0] / 2),
        qMax(0, dims[1] / 2),
        qMax(0, dims[2] / 2)
    }};

    double scalarRange[2] = {0.0, 0.0};
    m_imageData->GetScalarRange(scalarRange);
    m_scalarMinimum = scalarRange[0];
    m_scalarMaximum = scalarRange[1];
    chooseInitialWindow(m_imageData,
                        m_scalarMinimum,
                        m_scalarMaximum,
                        &m_windowLevel,
                        &m_windowWidth);

    createEmptyLabelVolume();
    if (seedDemoLabel) {
        createDemoLabelVolume();
    }

    setActiveOrientation(SliceOrientation::Axial, false);
    applyBackgroundColor();
    applySlicePresentation();
    rebuildVolumeSurface();

    emit imageLoaded();
    emit viewStateChanged();
    return true;
}

void ImageViewer::createEmptyLabelVolume()
{
    if (!m_imageData) {
        m_labelData = nullptr;
        return;
    }

    m_labelData = vtkSmartPointer<vtkImageData>::New();

    const int* dims = m_imageData->GetDimensions();
    double origin[3] = {0.0, 0.0, 0.0};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    m_labelData->SetDimensions(dims[0], dims[1], dims[2]);
    m_labelData->SetOrigin(origin);
    m_labelData->SetSpacing(spacing);
    m_labelData->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    if (m_labelData->GetPointData() && m_labelData->GetPointData()->GetScalars()) {
        m_labelData->GetPointData()->GetScalars()->FillComponent(0, 0.0);
    }
}

void ImageViewer::createDemoLabelVolume()
{
    if (!m_labelData) {
        return;
    }

    const int* dims = m_labelData->GetDimensions();
    const double cx = static_cast<double>(dims[0]) * 0.55;
    const double cy = static_cast<double>(dims[1]) * 0.46;
    const double cz = static_cast<double>(dims[2]) * 0.50;
    const double rx = 16.0;
    const double ry = 12.0;
    const double rz = 10.0;

    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                const double dx = (x - cx) / rx;
                const double dy = (y - cy) / ry;
                const double dz = (z - cz) / rz;
                if (dx * dx + dy * dy + dz * dz <= 1.0) {
                    auto* label = static_cast<unsigned char*>(m_labelData->GetScalarPointer(x, y, z));
                    if (label) {
                        *label = 1;
                    }
                }
            }
        }
    }

    if (m_labelData->GetPointData() && m_labelData->GetPointData()->GetScalars()) {
        m_labelData->GetPointData()->GetScalars()->Modified();
    }
    m_labelData->Modified();
}

void ImageViewer::applyWindowLevelInternal(int level, int width, bool notify)
{
    const int minimumLevel = static_cast<int>(std::floor(m_scalarMinimum));
    const int maximumLevel = static_cast<int>(std::ceil(m_scalarMaximum));
    const int maximumWidth = qMax(1, maximumLevel - minimumLevel);

    const int newLevel = m_imageData ? qBound(minimumLevel, level, maximumLevel) : level;
    const int newWidth = m_imageData ? qBound(1, width, maximumWidth) : qMax(1, width);

    if (newLevel == m_windowLevel && newWidth == m_windowWidth) {
        return;
    }

    m_windowLevel = newLevel;
    m_windowWidth = newWidth;
    applySlicePresentation();

    if (notify) {
        emit windowLevelChanged(m_windowLevel, m_windowWidth);
    }
}

void ImageViewer::applyBackgroundColor()
{
    if (m_axialView) {
        m_axialView->setBackgroundColor(m_backgroundColor);
    }
    if (m_coronalView) {
        m_coronalView->setBackgroundColor(m_backgroundColor);
    }
    if (m_sagittalView) {
        m_sagittalView->setBackgroundColor(m_backgroundColor);
    }
    applyVolumePresentation();
}

void ImageViewer::applySlicePresentation()
{
    const auto syncSlice = [this](MprSliceView* view, bool active) {
        if (!view) {
            return;
        }

        view->setVolumes(m_imageData, m_labelData);
        view->setCrosshair(m_crosshair);
        view->setWindowLevelWidth(m_windowLevel, m_windowWidth);
        view->setBackgroundColor(m_backgroundColor);
        view->setZoomPercent(m_zoomLevel);
        view->setLabelVisible(m_labelVisible);
        view->setLabelOpacity(m_labelOpacity);
        view->setBrushRadius(m_brushRadius);
        view->setBrushEditingEnabled(m_brushEditingEnabled);
        view->setEraseMode(m_eraseMode);
        view->setActive(active);
    };

    syncSlice(m_axialView, m_currentOrientation == SliceOrientation::Axial);
    syncSlice(m_coronalView, m_currentOrientation == SliceOrientation::Coronal);
    syncSlice(m_sagittalView, m_currentOrientation == SliceOrientation::Sagittal);
}

void ImageViewer::applyVolumePresentation()
{
    if (!m_volumeView) {
        return;
    }

    m_volumeView->setCrosshair(m_crosshair);
    m_volumeView->setBackgroundColor(m_backgroundColor);
    m_volumeView->setLabelVisible(m_labelVisible);
    m_volumeView->setLabelOpacity(m_labelOpacity);
}

void ImageViewer::rebuildVolumeSurface()
{
    if (!m_volumeView) {
        return;
    }

    m_volumeView->setLabelData(m_labelData);
    applyVolumePresentation();
}

void ImageViewer::scheduleVolumeRebuild()
{
    if (m_labelRefreshTimer) {
        m_labelRefreshTimer->start();
    }
}

void ImageViewer::setActiveOrientation(SliceOrientation orientation, bool notify)
{
    if (m_currentOrientation == orientation) {
        applySlicePresentation();
        return;
    }

    m_currentOrientation = orientation;
    applySlicePresentation();

    if (notify) {
        emit viewStateChanged();
    }
}

std::array<int, 3> ImageViewer::clampedCrosshair(const std::array<int, 3>& crosshair) const
{
    if (!m_imageData) {
        return crosshair;
    }

    const int* dims = m_imageData->GetDimensions();
    return {{
        qBound(0, crosshair[0], qMax(0, dims[0] - 1)),
        qBound(0, crosshair[1], qMax(0, dims[1] - 1)),
        qBound(0, crosshair[2], qMax(0, dims[2] - 1))
    }};
}

void ImageViewer::updateCrosshair(const std::array<int, 3>& crosshair, bool notify)
{
    const std::array<int, 3> nextCrosshair = clampedCrosshair(crosshair);
    if (nextCrosshair == m_crosshair) {
        return;
    }

    m_crosshair = nextCrosshair;
    applySlicePresentation();
    applyVolumePresentation();

    if (notify) {
        emit viewStateChanged();
    }
}

QImage ImageViewer::currentSliceSnapshot() const
{
    switch (m_currentOrientation) {
    case SliceOrientation::Axial:
        return m_axialView ? m_axialView->snapshotImage() : QImage();
    case SliceOrientation::Coronal:
        return m_coronalView ? m_coronalView->snapshotImage() : QImage();
    case SliceOrientation::Sagittal:
        return m_sagittalView ? m_sagittalView->snapshotImage() : QImage();
    }

    return {};
}
