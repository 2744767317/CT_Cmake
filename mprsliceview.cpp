#include "mprsliceview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtGlobal>

#include <cmath>

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>

namespace
{
unsigned char applyWindow(double value, double windowLevel, double windowWidth)
{
    const double safeWindowWidth = std::max(windowWidth, 1.0);
    const double windowMin = windowLevel - safeWindowWidth * 0.5;
    const double windowMax = windowLevel + safeWindowWidth * 0.5;

    if (value <= windowMin) {
        return 0;
    }
    if (value >= windowMax) {
        return 255;
    }

    const double normalized = (value - windowMin) / (windowMax - windowMin);
    return static_cast<unsigned char>(qBound(0, qRound(normalized * 255.0), 255));
}

QString orientationTitle(SliceOrientation orientation)
{
    switch (orientation) {
    case SliceOrientation::Axial:
        return QStringLiteral("Axial");
    case SliceOrientation::Coronal:
        return QStringLiteral("Coronal");
    case SliceOrientation::Sagittal:
        return QStringLiteral("Sagittal");
    }

    return QStringLiteral("Unknown");
}
}

MprSliceView::MprSliceView(SliceOrientation orientation, QWidget* parent)
    : QWidget(parent)
    , m_orientation(orientation)
{
    setMouseTracking(true);
    setAutoFillBackground(false);
}

void MprSliceView::setVolumes(vtkSmartPointer<vtkImageData> imageData,
                              vtkSmartPointer<vtkImageData> labelData)
{
    m_imageData = imageData;
    m_labelData = labelData;
    m_crosshair = clampedCrosshair(m_crosshair);
    invalidateView();
}

void MprSliceView::setCrosshair(const std::array<int, 3>& crosshair)
{
    m_crosshair = clampedCrosshair(crosshair);
    invalidateView();
}

void MprSliceView::setWindowLevelWidth(int level, int width)
{
    m_windowLevel = level;
    m_windowWidth = qMax(1, width);
    invalidateView();
}

void MprSliceView::setBackgroundColor(const QColor& color)
{
    m_backgroundColor = color;
    invalidateView();
}

void MprSliceView::setZoomPercent(int zoomPercent)
{
    m_zoomPercent = qMax(10, zoomPercent);
    invalidateView();
}

void MprSliceView::setLabelVisible(bool visible)
{
    m_labelVisible = visible;
    invalidateView();
}

void MprSliceView::setLabelOpacity(double opacity)
{
    m_labelOpacity = qBound(0.0, opacity, 1.0);
    invalidateView();
}

void MprSliceView::setBrushRadius(int radius)
{
    m_brushRadius = qMax(1, radius);
}

void MprSliceView::setBrushEditingEnabled(bool enabled)
{
    m_brushEditingEnabled = enabled;
}

void MprSliceView::setEraseMode(bool eraseMode)
{
    m_eraseMode = eraseMode;
}

void MprSliceView::setActive(bool active)
{
    m_active = active;
    update();
}

SliceOrientation MprSliceView::orientation() const
{
    return m_orientation;
}

int MprSliceView::sliceIndex() const
{
    switch (m_orientation) {
    case SliceOrientation::Axial:
        return m_crosshair[2];
    case SliceOrientation::Coronal:
        return m_crosshair[1];
    case SliceOrientation::Sagittal:
        return m_crosshair[0];
    }

    return 0;
}

int MprSliceView::sliceCount() const
{
    if (!m_imageData) {
        return 0;
    }

    const int* dims = m_imageData->GetDimensions();
    switch (m_orientation) {
    case SliceOrientation::Axial:
        return dims[2];
    case SliceOrientation::Coronal:
        return dims[1];
    case SliceOrientation::Sagittal:
        return dims[0];
    }

    return 0;
}

int MprSliceView::zoomPercent() const
{
    return m_zoomPercent;
}

QImage MprSliceView::snapshotImage() const
{
    rebuildCache();
    return m_cachedCanvas;
}

void MprSliceView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    rebuildCache();

    QPainter painter(this);
    painter.drawImage(rect(), m_cachedCanvas);

    const QColor borderColor = m_active ? QColor("#38bdf8") : QColor("#334155");
    painter.setPen(QPen(borderColor, m_active ? 3.0 : 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(1, 1, -2, -2));

    painter.setPen(QColor("#e2e8f0"));
    painter.drawText(QRect(10, 8, width() - 20, 20),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("%1 | Slice %2/%3")
                         .arg(orientationTitle(m_orientation))
                         .arg(sliceIndex())
                         .arg(qMax(0, sliceCount() - 1)));

    if (m_brushEditingEnabled) {
        painter.setPen(QColor("#fbbf24"));
        painter.drawText(QRect(10, height() - 28, width() - 20, 20),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         m_eraseMode ? QStringLiteral("Erase") : QStringLiteral("Brush"));
    }
}

void MprSliceView::mousePressEvent(QMouseEvent* event)
{
    emit activated(m_orientation);

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_leftButtonPressed = true;

    const VolumeIndex index = mapWidgetPointToVolume(event->pos());
    if (!index.valid) {
        return;
    }

    const std::array<int, 3> crosshair{{index.x, index.y, index.z}};
    emit crosshairChanged(crosshair);

    if (m_brushEditingEnabled && applyBrushStroke(index)) {
        emit labelModified();
    }
}

void MprSliceView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_leftButtonPressed) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const VolumeIndex index = mapWidgetPointToVolume(event->pos());
    if (!index.valid) {
        return;
    }

    const std::array<int, 3> crosshair{{index.x, index.y, index.z}};
    emit crosshairChanged(crosshair);

    if (m_brushEditingEnabled && applyBrushStroke(index)) {
        emit labelModified();
    }
}

void MprSliceView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_leftButtonPressed = false;
    }

    QWidget::mouseReleaseEvent(event);
}

void MprSliceView::wheelEvent(QWheelEvent* event)
{
    if (!m_imageData) {
        QWidget::wheelEvent(event);
        return;
    }

    emit activated(m_orientation);

    const int step = event->angleDelta().y() > 0 ? 1 : (event->angleDelta().y() < 0 ? -1 : 0);
    if (step == 0) {
        event->accept();
        return;
    }

    std::array<int, 3> nextCrosshair = m_crosshair;
    switch (m_orientation) {
    case SliceOrientation::Axial:
        nextCrosshair[2] += step;
        break;
    case SliceOrientation::Coronal:
        nextCrosshair[1] += step;
        break;
    case SliceOrientation::Sagittal:
        nextCrosshair[0] += step;
        break;
    }

    emit crosshairChanged(clampedCrosshair(nextCrosshair));
    event->accept();
}

QSize MprSliceView::minimumSizeHint() const
{
    return QSize(220, 220);
}

void MprSliceView::invalidateView()
{
    m_cacheDirty = true;
    update();
}

void MprSliceView::rebuildCache() const
{
    if (!m_cacheDirty && m_cachedCanvas.size() == size()) {
        return;
    }

    const QSize canvasSize = size().isValid() ? size() : QSize(320, 320);
    QImage canvas(canvasSize, QImage::Format_RGB32);
    canvas.fill(m_backgroundColor);

    m_cachedImageRect = QRect();

    if (m_imageData) {
        const QSize sourceSize = sourceSliceSize();
        if (sourceSize.width() > 0 && sourceSize.height() > 0) {
            QImage sliceImage(sourceSize, QImage::Format_ARGB32);

            for (int v = 0; v < sourceSize.height(); ++v) {
                QRgb* line = reinterpret_cast<QRgb*>(sliceImage.scanLine(v));
                for (int u = 0; u < sourceSize.width(); ++u) {
                    int x = 0;
                    int y = 0;
                    int z = 0;

                    switch (m_orientation) {
                    case SliceOrientation::Axial:
                        x = u;
                        y = v;
                        z = m_crosshair[2];
                        break;
                    case SliceOrientation::Coronal:
                        x = u;
                        y = m_crosshair[1];
                        z = sourceSize.height() - 1 - v;
                        break;
                    case SliceOrientation::Sagittal:
                        x = m_crosshair[0];
                        y = u;
                        z = sourceSize.height() - 1 - v;
                        break;
                    }

                    const unsigned char gray = applyWindow(
                        scalarAt(m_imageData, x, y, z),
                        static_cast<double>(m_windowLevel),
                        static_cast<double>(m_windowWidth));

                    QColor color(gray, gray, gray);
                    if (m_labelVisible && labelAt(x, y, z) > 0) {
                        const QColor overlay(236, 72, 153);
                        color.setRed(qRound(color.red() * (1.0 - m_labelOpacity) + overlay.red() * m_labelOpacity));
                        color.setGreen(qRound(color.green() * (1.0 - m_labelOpacity) + overlay.green() * m_labelOpacity));
                        color.setBlue(qRound(color.blue() * (1.0 - m_labelOpacity) + overlay.blue() * m_labelOpacity));
                    }

                    line[u] = color.rgb();
                }
            }

            m_cachedImageRect = targetImageRect(canvasSize);
            if (m_cachedImageRect.isValid()) {
                QPainter painter(&canvas);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawImage(m_cachedImageRect, sliceImage);

                const QPointF crossPoint = crosshairDisplayPoint(m_cachedImageRect);
                painter.setPen(QPen(QColor("#facc15"), 1.0, Qt::DashLine));
                painter.drawLine(QPointF(m_cachedImageRect.left(), crossPoint.y()),
                                 QPointF(m_cachedImageRect.right(), crossPoint.y()));
                painter.drawLine(QPointF(crossPoint.x(), m_cachedImageRect.top()),
                                 QPointF(crossPoint.x(), m_cachedImageRect.bottom()));
            }
        }
    }

    m_cachedCanvas = canvas;
    m_cacheDirty = false;
}

QSize MprSliceView::sourceSliceSize() const
{
    if (!m_imageData) {
        return {};
    }

    const int* dims = m_imageData->GetDimensions();
    switch (m_orientation) {
    case SliceOrientation::Axial:
        return QSize(dims[0], dims[1]);
    case SliceOrientation::Coronal:
        return QSize(dims[0], dims[2]);
    case SliceOrientation::Sagittal:
        return QSize(dims[1], dims[2]);
    }

    return {};
}

QRect MprSliceView::targetImageRect(const QSize& canvasSize) const
{
    const QSize sourceSize = sourceSliceSize();
    if (!sourceSize.isValid()) {
        return {};
    }

    QSize scaled = sourceSize;
    scaled.scale(canvasSize, Qt::KeepAspectRatio);

    const double zoomFactor = std::max(0.1, static_cast<double>(m_zoomPercent) / 100.0);
    scaled = QSize(qMax(1, qRound(static_cast<double>(scaled.width()) * zoomFactor)),
                   qMax(1, qRound(static_cast<double>(scaled.height()) * zoomFactor)));

    return QRect((canvasSize.width() - scaled.width()) / 2,
                 (canvasSize.height() - scaled.height()) / 2,
                 scaled.width(),
                 scaled.height());
}

QPointF MprSliceView::crosshairDisplayPoint(const QRect& drawRect) const
{
    const QSize sourceSize = sourceSliceSize();
    if (!drawRect.isValid() || !sourceSize.isValid()) {
        return {};
    }

    double u = 0.0;
    double v = 0.0;
    switch (m_orientation) {
    case SliceOrientation::Axial:
        u = static_cast<double>(m_crosshair[0]) + 0.5;
        v = static_cast<double>(m_crosshair[1]) + 0.5;
        break;
    case SliceOrientation::Coronal:
        u = static_cast<double>(m_crosshair[0]) + 0.5;
        v = static_cast<double>(sourceSize.height() - 1 - m_crosshair[2]) + 0.5;
        break;
    case SliceOrientation::Sagittal:
        u = static_cast<double>(m_crosshair[1]) + 0.5;
        v = static_cast<double>(sourceSize.height() - 1 - m_crosshair[2]) + 0.5;
        break;
    }

    const double x = drawRect.left() + (u / qMax(1, sourceSize.width())) * drawRect.width();
    const double y = drawRect.top() + (v / qMax(1, sourceSize.height())) * drawRect.height();
    return QPointF(x, y);
}

MprSliceView::VolumeIndex MprSliceView::mapWidgetPointToVolume(const QPoint& pos) const
{
    VolumeIndex index;
    if (!m_imageData) {
        return index;
    }

    const QRect drawRect = targetImageRect(size());
    if (!drawRect.contains(pos)) {
        return index;
    }

    const QSize sourceSize = sourceSliceSize();
    if (!sourceSize.isValid()) {
        return index;
    }

    const double xRatio = static_cast<double>(pos.x() - drawRect.left()) / qMax(1, drawRect.width());
    const double yRatio = static_cast<double>(pos.y() - drawRect.top()) / qMax(1, drawRect.height());
    const int u = qBound(0, static_cast<int>(std::floor(xRatio * sourceSize.width())), sourceSize.width() - 1);
    const int v = qBound(0, static_cast<int>(std::floor(yRatio * sourceSize.height())), sourceSize.height() - 1);

    switch (m_orientation) {
    case SliceOrientation::Axial:
        index.x = u;
        index.y = v;
        index.z = m_crosshair[2];
        break;
    case SliceOrientation::Coronal:
        index.x = u;
        index.y = m_crosshair[1];
        index.z = sourceSize.height() - 1 - v;
        break;
    case SliceOrientation::Sagittal:
        index.x = m_crosshair[0];
        index.y = u;
        index.z = sourceSize.height() - 1 - v;
        break;
    }

    index.valid = true;
    return index;
}

bool MprSliceView::applyBrushStroke(const VolumeIndex& center)
{
    if (!m_labelData || !center.valid) {
        return false;
    }

    const int* dims = m_labelData->GetDimensions();
    bool modified = false;
    const unsigned char value = m_eraseMode ? 0 : 1;
    const int radiusSquared = m_brushRadius * m_brushRadius;

    for (int dv = -m_brushRadius; dv <= m_brushRadius; ++dv) {
        for (int du = -m_brushRadius; du <= m_brushRadius; ++du) {
            if (du * du + dv * dv > radiusSquared) {
                continue;
            }

            int x = center.x;
            int y = center.y;
            int z = center.z;
            switch (m_orientation) {
            case SliceOrientation::Axial:
                x += du;
                y += dv;
                break;
            case SliceOrientation::Coronal:
                x += du;
                z += dv;
                break;
            case SliceOrientation::Sagittal:
                y += du;
                z += dv;
                break;
            }

            if (x < 0 || y < 0 || z < 0 ||
                x >= dims[0] || y >= dims[1] || z >= dims[2]) {
                continue;
            }

            auto* labelPtr = static_cast<unsigned char*>(m_labelData->GetScalarPointer(x, y, z));
            if (!labelPtr || *labelPtr == value) {
                continue;
            }

            *labelPtr = value;
            modified = true;
        }
    }

    if (modified) {
        if (m_labelData->GetPointData() && m_labelData->GetPointData()->GetScalars()) {
            m_labelData->GetPointData()->GetScalars()->Modified();
        }
        m_labelData->Modified();
        invalidateView();
    }

    return modified;
}

double MprSliceView::scalarAt(vtkImageData* imageData, int x, int y, int z) const
{
    return imageData ? imageData->GetScalarComponentAsDouble(x, y, z, 0) : 0.0;
}

unsigned char MprSliceView::labelAt(int x, int y, int z) const
{
    if (!m_labelData) {
        return 0;
    }

    auto* labelPtr = static_cast<unsigned char*>(m_labelData->GetScalarPointer(x, y, z));
    return labelPtr ? *labelPtr : 0;
}

std::array<int, 3> MprSliceView::clampedCrosshair(const std::array<int, 3>& crosshair) const
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
