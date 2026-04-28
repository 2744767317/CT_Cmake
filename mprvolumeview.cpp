#include "mprvolumeview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkMarchingCubes.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>
#include <vtkWindowToImageFilter.h>

namespace
{
QImage vtkRgbaImageToQImage(vtkImageData* imageData)
{
    if (!imageData) {
        return {};
    }

    const int* dims = imageData->GetDimensions();
    const int width = dims[0];
    const int height = dims[1];
    const int components = imageData->GetNumberOfScalarComponents();
    auto* pixels = static_cast<unsigned char*>(imageData->GetScalarPointer());
    if (!pixels || width <= 0 || height <= 0 || components <= 0) {
        return {};
    }

    QImage image(width, height, QImage::Format_ARGB32);
    const int stride = width * components;

    // VTK 的窗口抓图原点在左下角，Qt 图像原点在左上角，这里做一次垂直翻转。
    for (int y = 0; y < height; ++y) {
        const unsigned char* sourceLine = pixels + (height - 1 - y) * stride;
        QRgb* targetLine = reinterpret_cast<QRgb*>(image.scanLine(y));

        for (int x = 0; x < width; ++x) {
            const unsigned char* source = sourceLine + x * components;
            const unsigned char r = source[0];
            const unsigned char g = components > 1 ? source[1] : source[0];
            const unsigned char b = components > 2 ? source[2] : source[0];
            const unsigned char a = components > 3 ? source[3] : 255;
            targetLine[x] = qRgba(r, g, b, a);
        }
    }

    return image;
}
}

MprVolumeView::MprVolumeView(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAutoFillBackground(false);
}

void MprVolumeView::setLabelData(vtkSmartPointer<vtkImageData> labelData)
{
    if (m_labelData != labelData) {
        m_labelData = labelData;
        m_resetCameraOnNextSurface = true;
    }

    rebuildSurface();
}

void MprVolumeView::setCrosshair(const std::array<int, 3>& crosshair)
{
    if (m_crosshair == crosshair) {
        return;
    }

    m_crosshair = crosshair;
    updateMarker();
    renderToCache();
    update();
}

void MprVolumeView::setBackgroundColor(const QColor& color)
{
    if (m_backgroundColor == color) {
        return;
    }

    m_backgroundColor = color;
    if (m_renderer) {
        m_renderer->SetBackground(color.redF(), color.greenF(), color.blueF());
    }

    renderToCache();
    update();
}

void MprVolumeView::setLabelVisible(bool visible)
{
    if (m_labelVisible == visible) {
        return;
    }

    m_labelVisible = visible;

    if (m_surfaceActor) {
        m_surfaceActor->SetVisibility(m_hasVisibleSurface && m_labelVisible);
    }

    updateMarker();
    renderToCache();
    update();
}

void MprVolumeView::setLabelOpacity(double opacity)
{
    const double clampedOpacity = qBound(0.0, opacity, 1.0);
    if (qFuzzyCompare(m_labelOpacity, clampedOpacity)) {
        return;
    }

    m_labelOpacity = clampedOpacity;

    if (m_surfaceActor) {
        m_surfaceActor->GetProperty()->SetOpacity(m_labelOpacity);
    }

    renderToCache();
    update();
}

void MprVolumeView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    if (m_cachedImage.size() != size()) {
        renderToCache();
    }

    QPainter painter(this);
    painter.fillRect(rect(), m_backgroundColor);

    if (!m_cachedImage.isNull()) {
        painter.drawImage(rect(), m_cachedImage);
    }

    painter.setPen(QPen(QColor("#334155"), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(1, 1, -2, -2));

    painter.setPen(QColor("#e2e8f0"));
    painter.drawText(QRect(10, 8, width() - 20, 20),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("3D Label Preview"));

    if (!m_labelData) {
        painter.setPen(QColor("#94a3b8"));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No volume loaded"));
        return;
    }

    if (!m_labelVisible) {
        painter.setPen(QColor("#94a3b8"));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Label hidden"));
        return;
    }

    if (!m_hasVisibleSurface) {
        painter.setPen(QColor("#94a3b8"));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Paint labels to generate a 3D surface"));
    }
}

void MprVolumeView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    if (m_renderWindow) {
        m_renderWindow->SetSize(qMax(1, width()), qMax(1, height()));
    }

    renderToCache();
}

void MprVolumeView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_leftButtonPressed = true;
        m_lastMousePos = event->pos();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void MprVolumeView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_leftButtonPressed || !m_renderer) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    vtkCamera* camera = m_renderer->GetActiveCamera();
    if (!camera) {
        return;
    }

    camera->Azimuth(-delta.x() * 0.6);
    camera->Elevation(-delta.y() * 0.6);
    camera->OrthogonalizeViewUp();
    m_renderer->ResetCameraClippingRange();

    renderToCache();
    update();
}

void MprVolumeView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_leftButtonPressed = false;
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void MprVolumeView::wheelEvent(QWheelEvent* event)
{
    if (!m_renderer || event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    vtkCamera* camera = m_renderer->GetActiveCamera();
    if (!camera) {
        return;
    }

    const double factor = event->angleDelta().y() > 0 ? 1.15 : (1.0 / 1.15);
    camera->Dolly(factor);
    m_renderer->ResetCameraClippingRange();

    renderToCache();
    update();
    event->accept();
}

QSize MprVolumeView::minimumSizeHint() const
{
    return QSize(220, 220);
}

void MprVolumeView::initializeScene()
{
    if (m_initialized) {
        return;
    }

    m_renderer = vtkSmartPointer<vtkRenderer>::New();
    m_renderer->SetBackground(m_backgroundColor.redF(),
                              m_backgroundColor.greenF(),
                              m_backgroundColor.blueF());

    m_renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    m_renderWindow->SetOffScreenRendering(true);
    m_renderWindow->AddRenderer(m_renderer);
    m_renderWindow->SetSize(qMax(1, width()), qMax(1, height()));

    m_surfaceExtractor = vtkSmartPointer<vtkMarchingCubes>::New();
    m_surfaceExtractor->ComputeNormalsOn();
    m_surfaceExtractor->ComputeGradientsOff();
    m_surfaceExtractor->SetValue(0, 0.5);

    m_surfaceMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_surfaceMapper->SetInputConnection(m_surfaceExtractor->GetOutputPort());
    m_surfaceMapper->ScalarVisibilityOff();

    m_surfaceActor = vtkSmartPointer<vtkActor>::New();
    m_surfaceActor->SetMapper(m_surfaceMapper);
    m_surfaceActor->GetProperty()->SetColor(0.956, 0.447, 0.713);
    m_surfaceActor->GetProperty()->SetOpacity(m_labelOpacity);
    m_surfaceActor->GetProperty()->SetSpecular(0.15);
    m_surfaceActor->GetProperty()->SetSpecularPower(18.0);
    m_surfaceActor->VisibilityOff();
    m_renderer->AddActor(m_surfaceActor);

    m_markerSource = vtkSmartPointer<vtkSphereSource>::New();
    m_markerSource->SetThetaResolution(18);
    m_markerSource->SetPhiResolution(18);

    m_markerMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_markerMapper->SetInputConnection(m_markerSource->GetOutputPort());

    m_markerActor = vtkSmartPointer<vtkActor>::New();
    m_markerActor->SetMapper(m_markerMapper);
    m_markerActor->GetProperty()->SetColor(0.980, 0.792, 0.082);
    m_markerActor->GetProperty()->SetOpacity(0.95);
    m_markerActor->VisibilityOff();
    m_renderer->AddActor(m_markerActor);

    m_windowToImage = vtkSmartPointer<vtkWindowToImageFilter>::New();
    m_windowToImage->SetInput(m_renderWindow);
    m_windowToImage->SetInputBufferTypeToRGBA();
    m_windowToImage->ReadFrontBufferOff();

    m_initialized = true;
}

void MprVolumeView::rebuildSurface()
{
    initializeScene();

    if (!m_labelData || m_labelData->GetNumberOfPoints() == 0) {
        m_hasVisibleSurface = false;
        if (m_surfaceActor) {
            m_surfaceActor->VisibilityOff();
        }
        updateMarker();
        renderToCache();
        update();
        return;
    }

    m_surfaceExtractor->SetInputData(m_labelData);
    m_surfaceExtractor->Update();

    vtkPolyData* polyData = m_surfaceExtractor->GetOutput();
    const bool hasGeometry = polyData && polyData->GetNumberOfPoints() > 0;
    m_hasVisibleSurface = hasGeometry;

    m_surfaceActor->GetProperty()->SetOpacity(m_labelOpacity);
    m_surfaceActor->SetVisibility(hasGeometry && m_labelVisible);

    if (hasGeometry && m_resetCameraOnNextSurface) {
        m_renderer->ResetCamera();
        vtkCamera* camera = m_renderer->GetActiveCamera();
        if (camera) {
            camera->Azimuth(-35.0);
            camera->Elevation(25.0);
            camera->Dolly(1.1);
            camera->OrthogonalizeViewUp();
        }
        m_renderer->ResetCameraClippingRange();
        m_resetCameraOnNextSurface = false;
    } else if (!hasGeometry) {
        m_resetCameraOnNextSurface = true;
    }

    updateMarker();
    renderToCache();
    update();
}

void MprVolumeView::updateMarker()
{
    if (!m_markerActor) {
        return;
    }

    if (!m_labelData || !m_hasVisibleSurface || !m_labelVisible) {
        m_markerActor->VisibilityOff();
        return;
    }

    const int* dims = m_labelData->GetDimensions();
    const std::array<int, 3> clamped = {{
        qBound(0, m_crosshair[0], qMax(0, dims[0] - 1)),
        qBound(0, m_crosshair[1], qMax(0, dims[1] - 1)),
        qBound(0, m_crosshair[2], qMax(0, dims[2] - 1))
    }};

    double origin[3] = {0.0, 0.0, 0.0};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_labelData->GetOrigin(origin);
    m_labelData->GetSpacing(spacing);

    const double center[3] = {
        origin[0] + spacing[0] * static_cast<double>(clamped[0]),
        origin[1] + spacing[1] * static_cast<double>(clamped[1]),
        origin[2] + spacing[2] * static_cast<double>(clamped[2])
    };

    const double radius = std::max({std::abs(spacing[0]), std::abs(spacing[1]), std::abs(spacing[2]), 1.0}) * 1.5;

    m_markerSource->SetCenter(center);
    m_markerSource->SetRadius(radius);
    m_markerSource->Update();
    m_markerActor->VisibilityOn();
}

void MprVolumeView::renderToCache()
{
    if (!isVisible() && width() <= 0 && height() <= 0) {
        return;
    }

    initializeScene();

    if (!m_renderWindow || !m_windowToImage) {
        return;
    }

    const int targetWidth = qMax(1, width());
    const int targetHeight = qMax(1, height());
    m_renderWindow->SetSize(targetWidth, targetHeight);
    m_renderWindow->Render();

    m_windowToImage->Modified();
    m_windowToImage->Update();
    m_cachedImage = vtkRgbaImageToQImage(m_windowToImage->GetOutput());
}
