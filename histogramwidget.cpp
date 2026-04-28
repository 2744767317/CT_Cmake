#include "histogramwidget.h"

#include <QBrush>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QToolTip>
#include <QtGlobal>

#include <vtkDataArray.h>
#include <vtkExtractVOI.h>
#include <vtkPointData.h>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kHistogramBins = 256;
constexpr int kPaddingLeft = 40;
constexpr int kPaddingRight = 14;
constexpr int kPaddingTop = 18;
constexpr int kPaddingBottom = 28;

QRectF histogramPlotRect(const QSize& size)
{
    return QRectF(kPaddingLeft,
                  kPaddingTop,
                  qMax(1, size.width() - kPaddingLeft - kPaddingRight),
                  qMax(1, size.height() - kPaddingTop - kPaddingBottom));
}

QString formatValue(double value)
{
    if (std::abs(value) >= 1000.0) {
        return QString::number(value, 'f', 0);
    }
    if (std::abs(value) >= 100.0) {
        return QString::number(value, 'f', 0);
    }
    return QString::number(value, 'f', 1);
}
}

HistogramWidget::HistogramWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
}

void HistogramWidget::setImageData(vtkSmartPointer<vtkImageData> imageData)
{
    m_imageData = imageData;
    calculateHistogram();
    update();
}

void HistogramWidget::setCurrentSlice(int slice)
{
    m_currentSlice = slice;
    calculateHistogram();
    update();
}

void HistogramWidget::setSliceOrientation(SliceOrientation orientation)
{
    m_sliceOrientation = orientation;
    calculateHistogram();
    update();
}

void HistogramWidget::setScaleMode(ScaleMode mode)
{
    m_scaleMode = mode;
    update();
}

void HistogramWidget::setWindowLevel(int level, int width)
{
    m_windowLevel = level;
    m_windowWidth = qMax(1, width);
    update();
}

void HistogramWidget::setDarkTheme(bool darkTheme)
{
    if (m_darkTheme == darkTheme) {
        return;
    }

    m_darkTheme = darkTheme;
    update();
}

void HistogramWidget::calculateHistogram()
{
    m_histogram.clear();
    m_minValue = 0.0;
    m_maxValue = 255.0;

    if (!m_imageData || !m_imageData->GetPointData()) {
        return;
    }

    vtkSmartPointer<vtkImageData> sliceData = m_imageData;
    const int* dimensions = m_imageData->GetDimensions();

    auto extractVOI = vtkSmartPointer<vtkExtractVOI>::New();
    extractVOI->SetInputData(m_imageData);

    switch (m_sliceOrientation) {
    case SliceOrientation::Axial: {
        const int safeSlice = qBound(0, m_currentSlice, qMax(0, dimensions[2] - 1));
        extractVOI->SetVOI(0, qMax(0, dimensions[0] - 1),
                           0, qMax(0, dimensions[1] - 1),
                           safeSlice, safeSlice);
        break;
    }
    case SliceOrientation::Coronal: {
        const int safeSlice = qBound(0, m_currentSlice, qMax(0, dimensions[1] - 1));
        extractVOI->SetVOI(0, qMax(0, dimensions[0] - 1),
                           safeSlice, safeSlice,
                           0, qMax(0, dimensions[2] - 1));
        break;
    }
    case SliceOrientation::Sagittal: {
        const int safeSlice = qBound(0, m_currentSlice, qMax(0, dimensions[0] - 1));
        extractVOI->SetVOI(safeSlice, safeSlice,
                           0, qMax(0, dimensions[1] - 1),
                           0, qMax(0, dimensions[2] - 1));
        break;
    }
    }

    extractVOI->Update();
    sliceData = extractVOI->GetOutput();

    vtkDataArray* scalars = sliceData->GetPointData() ? sliceData->GetPointData()->GetScalars() : nullptr;
    if (!scalars) {
        return;
    }

    double range[2] = {0.0, 0.0};
    scalars->GetRange(range);
    m_minValue = range[0];
    m_maxValue = range[1];

    if (!std::isfinite(m_minValue) || !std::isfinite(m_maxValue)) {
        m_minValue = 0.0;
        m_maxValue = 255.0;
        return;
    }

    if (m_maxValue <= m_minValue) {
        m_maxValue = m_minValue + 1.0;
    }

    m_histogram.fill(0, kHistogramBins);
    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    const double valueRange = m_maxValue - m_minValue;

    for (vtkIdType index = 0; index < tupleCount; ++index) {
        const double value = scalars->GetComponent(index, 0);
        if (!std::isfinite(value)) {
            continue;
        }

        const double normalized = (value - m_minValue) / valueRange;
        const int bin = qBound(0,
                               static_cast<int>(normalized * static_cast<double>(kHistogramBins - 1)),
                               kHistogramBins - 1);
        ++m_histogram[bin];
    }
}

void HistogramWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor canvasColor = m_darkTheme ? QColor("#0f172a") : QColor("#f7f9fc");
    const QColor frameColor = m_darkTheme ? QColor("#334155") : QColor("#d5dbe5");
    const QColor emptyTextColor = m_darkTheme ? QColor("#94a3b8") : QColor("#64748b");

    painter.fillRect(rect(), canvasColor);

    const QRectF plotRect = histogramPlotRect(size());
    painter.setPen(QPen(frameColor, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(plotRect.adjusted(-6, -6, 6, 6), 8, 8);

    if (m_histogram.isEmpty()) {
        painter.setPen(emptyTextColor);
        painter.drawText(rect(), Qt::AlignCenter, tr("No histogram"));
        return;
    }

    drawHistogram(&painter);
    drawWindowLevelRange(&painter);
}

void HistogramWidget::drawHistogram(QPainter* painter)
{
    const QRectF plotRect = histogramPlotRect(size());
    if (plotRect.width() <= 1.0 || plotRect.height() <= 1.0) {
        return;
    }

    int maxCount = 0;
    QVector<int> nonZeroCounts;
    nonZeroCounts.reserve(m_histogram.size());
    for (int count : m_histogram) {
        maxCount = qMax(maxCount, count);
        if (count > 0) {
            nonZeroCounts.push_back(count);
        }
    }

    if (maxCount <= 0) {
        return;
    }

    std::sort(nonZeroCounts.begin(), nonZeroCounts.end());
    int displayMaxCount = maxCount;
    if (m_scaleMode == ScaleMode::Linear && nonZeroCounts.size() > 1) {
        const int secondLargest = nonZeroCounts.at(nonZeroCounts.size() - 2);
        const int p90Index = qBound(0,
                                    static_cast<int>(std::floor((nonZeroCounts.size() - 1) * 0.9)),
                                    nonZeroCounts.size() - 1);
        const int p90 = nonZeroCounts.at(p90Index);
        displayMaxCount = qMax(1, qMin(maxCount, qMax(secondLargest, p90) * 3));
    }

    painter->save();

    const QColor gridColor = m_darkTheme ? QColor("#243244") : QColor("#e2e8f0");
    const QColor fillTopColor = m_darkTheme ? QColor(56, 189, 248, 160) : QColor(37, 99, 235, 180);
    const QColor fillBottomColor = m_darkTheme ? QColor(14, 165, 233, 36) : QColor(96, 165, 250, 40);
    const QColor curveColor = m_darkTheme ? QColor("#60a5fa") : QColor("#1d4ed8");
    const QColor axisColor = m_darkTheme ? QColor("#64748b") : QColor("#475569");
    const QColor textColor = m_darkTheme ? QColor("#cbd5e1") : QColor("#334155");

    painter->setPen(QPen(gridColor, 1.0));
    for (int line = 0; line <= 4; ++line) {
        const qreal y = plotRect.bottom() - (plotRect.height() * line / 4.0);
        painter->drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));
    }

    QPainterPath areaPath;
    areaPath.moveTo(plotRect.bottomLeft());

    for (int index = 0; index < m_histogram.size(); ++index) {
        const int count = m_histogram.at(index);
        double heightRatio = 0.0;

        if (m_scaleMode == ScaleMode::Logarithmic) {
            heightRatio = std::log10(static_cast<double>(count) + 1.0) /
                          std::log10(static_cast<double>(maxCount) + 1.0);
        } else {
            const int clippedCount = qMin(count, displayMaxCount);
            heightRatio = static_cast<double>(clippedCount) / static_cast<double>(displayMaxCount);
        }

        const qreal x = plotRect.left() +
                        (static_cast<qreal>(index) / qMax(1, m_histogram.size() - 1)) * plotRect.width();
        const qreal y = plotRect.bottom() - heightRatio * plotRect.height();
        areaPath.lineTo(QPointF(x, y));
    }

    areaPath.lineTo(plotRect.bottomRight());
    areaPath.closeSubpath();

    QLinearGradient gradient(plotRect.topLeft(), plotRect.bottomLeft());
    gradient.setColorAt(0.0, fillTopColor);
    gradient.setColorAt(1.0, fillBottomColor);

    painter->fillPath(areaPath, gradient);
    painter->setPen(QPen(curveColor, 1.4));
    painter->drawPath(areaPath);

    painter->setPen(QPen(axisColor, 1.2));
    painter->drawLine(plotRect.bottomLeft(), plotRect.bottomRight());
    painter->drawLine(plotRect.bottomLeft(), plotRect.topLeft());

    painter->setPen(textColor);
    painter->drawText(QRectF(plotRect.left(), plotRect.bottom() + 4.0, 80.0, 18.0),
                      Qt::AlignLeft | Qt::AlignTop,
                      formatValue(m_minValue));
    painter->drawText(QRectF(plotRect.right() - 80.0, plotRect.bottom() + 4.0, 80.0, 18.0),
                      Qt::AlignRight | Qt::AlignTop,
                      formatValue(m_maxValue));

    painter->restore();
}

void HistogramWidget::drawWindowLevelRange(QPainter* painter)
{
    const QRectF plotRect = histogramPlotRect(size());
    const double valueRange = m_maxValue - m_minValue;
    if (valueRange <= 0.0) {
        return;
    }

    const double windowMin = static_cast<double>(m_windowLevel) - static_cast<double>(m_windowWidth) * 0.5;
    const double windowMax = static_cast<double>(m_windowLevel) + static_cast<double>(m_windowWidth) * 0.5;

    const double minRatio = std::clamp((windowMin - m_minValue) / valueRange, 0.0, 1.0);
    const double maxRatio = std::clamp((windowMax - m_minValue) / valueRange, 0.0, 1.0);
    const double centerRatio = std::clamp((static_cast<double>(m_windowLevel) - m_minValue) / valueRange, 0.0, 1.0);

    const qreal x1 = plotRect.left() + minRatio * plotRect.width();
    const qreal x2 = plotRect.left() + maxRatio * plotRect.width();
    const qreal centerX = plotRect.left() + centerRatio * plotRect.width();

    painter->save();

    const QColor rangeFillColor = m_darkTheme ? QColor(56, 189, 248, 36) : QColor(59, 130, 246, 36);
    const QColor boundaryColor = m_darkTheme ? QColor("#38bdf8") : QColor("#2563eb");
    const QColor centerLineColor = m_darkTheme ? QColor("#e2e8f0") : QColor("#0f172a");
    const QColor labelColor = m_darkTheme ? QColor("#f8fafc") : QColor("#0f172a");

    painter->setBrush(rangeFillColor);
    painter->setPen(Qt::NoPen);
    painter->drawRect(QRectF(QPointF(x1, plotRect.top()), QPointF(x2, plotRect.bottom())));

    painter->setPen(QPen(boundaryColor, 1.2));
    painter->drawLine(QPointF(x1, plotRect.top()), QPointF(x1, plotRect.bottom()));
    painter->drawLine(QPointF(x2, plotRect.top()), QPointF(x2, plotRect.bottom()));

    painter->setPen(QPen(centerLineColor, 1.0, Qt::DashLine));
    painter->drawLine(QPointF(centerX, plotRect.top()), QPointF(centerX, plotRect.bottom()));

    painter->setPen(labelColor);
    painter->drawText(QRectF(plotRect.left(), 2.0, plotRect.width(), 14.0),
                      Qt::AlignRight | Qt::AlignVCenter,
                      QString("WL %1  WW %2").arg(m_windowLevel).arg(m_windowWidth));

    painter->restore();
}

void HistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_histogram.isEmpty()) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QRectF plotRect = histogramPlotRect(size());
    if (!plotRect.adjusted(-8, -8, 8, 8).contains(event->pos())) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_mousePressed = true;
    m_lastMousePos = event->pos();

    const double valueRange = m_maxValue - m_minValue;
    const double windowMin = static_cast<double>(m_windowLevel) - static_cast<double>(m_windowWidth) * 0.5;
    const double windowMax = static_cast<double>(m_windowLevel) + static_cast<double>(m_windowWidth) * 0.5;
    const double x1 = plotRect.left() + std::clamp((windowMin - m_minValue) / valueRange, 0.0, 1.0) * plotRect.width();
    const double x2 = plotRect.left() + std::clamp((windowMax - m_minValue) / valueRange, 0.0, 1.0) * plotRect.width();

    if (std::abs(event->pos().x() - x1) <= 6.0) {
        m_draggingMode = DraggingMode::WindowMin;
    } else if (std::abs(event->pos().x() - x2) <= 6.0) {
        m_draggingMode = DraggingMode::WindowMax;
    } else if (event->pos().x() >= x1 && event->pos().x() <= x2) {
        m_draggingMode = DraggingMode::WindowLevel;
    } else {
        m_draggingMode = DraggingMode::WindowLevel;
        const double clickRatio = std::clamp((event->pos().x() - plotRect.left()) / plotRect.width(), 0.0, 1.0);
        m_windowLevel = qRound(m_minValue + clickRatio * valueRange);
        m_windowWidth = qMax(1, qRound(valueRange * 0.15));
        emit windowLevelChanged(m_windowLevel, m_windowWidth);
        update();
    }
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QRectF plotRect = histogramPlotRect(size());
    const double valueRange = m_maxValue - m_minValue;

    if (m_mousePressed && m_draggingMode != DraggingMode::None && valueRange > 0.0) {
        const int deltaX = event->pos().x() - m_lastMousePos.x();
        m_lastMousePos = event->pos();

        const double deltaValue = (static_cast<double>(deltaX) / plotRect.width()) * valueRange;
        const int currentWindowMin = m_windowLevel - m_windowWidth / 2;
        const int currentWindowMax = m_windowLevel + m_windowWidth / 2;

        if (m_draggingMode == DraggingMode::WindowLevel) {
            m_windowLevel += qRound(deltaValue);
        } else if (m_draggingMode == DraggingMode::WindowMin) {
            const int newWindowMin = currentWindowMin + qRound(deltaValue);
            const int newWindowWidth = currentWindowMax - newWindowMin;
            if (newWindowWidth > 1) {
                m_windowWidth = newWindowWidth;
                m_windowLevel = (newWindowMin + currentWindowMax) / 2;
            }
        } else if (m_draggingMode == DraggingMode::WindowMax) {
            const int newWindowMax = currentWindowMax + qRound(deltaValue);
            const int newWindowWidth = newWindowMax - currentWindowMin;
            if (newWindowWidth > 1) {
                m_windowWidth = newWindowWidth;
                m_windowLevel = (currentWindowMin + newWindowMax) / 2;
            }
        }

        m_windowLevel = qBound(static_cast<int>(std::floor(m_minValue)),
                               m_windowLevel,
                               static_cast<int>(std::ceil(m_maxValue)));
        m_windowWidth = qMax(1, m_windowWidth);

        emit windowLevelChanged(m_windowLevel, m_windowWidth);
        update();
    }

    if (!m_histogram.isEmpty() && plotRect.contains(event->pos()) && plotRect.width() > 0.0) {
        const double ratio = std::clamp((event->pos().x() - plotRect.left()) / plotRect.width(), 0.0, 1.0);
        const int bin = qBound(0,
                               static_cast<int>(std::floor(ratio * static_cast<double>(m_histogram.size()))),
                               m_histogram.size() - 1);
        const double value = m_minValue +
                             ((static_cast<double>(bin) + 0.5) / static_cast<double>(m_histogram.size())) *
                                 (m_maxValue - m_minValue);
        QToolTip::showText(event->globalPos(),
                           QString("Value: %1\nCount: %2")
                               .arg(formatValue(value))
                               .arg(m_histogram.at(bin)));
    } else {
        QToolTip::hideText();
    }
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_mousePressed = false;
        m_draggingMode = DraggingMode::None;
    }

    QWidget::mouseReleaseEvent(event);
}
