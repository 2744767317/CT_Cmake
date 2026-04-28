#include "acquisitionsaveworker.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QtGlobal>

AcquisitionSaveWorker::AcquisitionSaveWorker(QObject* parent)
    : QObject(parent)
{
}

void AcquisitionSaveWorker::saveFrame(const QString& sessionDir,
                                      int sequenceNo,
                                      const QByteArray& image,
                                      int width,
                                      int height,
                                      int bytesPerPixel)
{
    // Each frame is written as proj_XXXXXX.mha so later reconstruction can sort lexicographically.
    QDir dir(sessionDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        emit saveFailed(QString("Failed to create acquisition session directory: %1").arg(sessionDir));
        return;
    }

    const QString fileName = QString("proj_%1.mha").arg(sequenceNo, 6, 10, QChar('0'));
    const QString filePath = dir.filePath(fileName);

    QString errMsg;
    if (!writeMhaFile(filePath, image, width, height, bytesPerPixel, errMsg)) {
        emit saveFailed(errMsg);
        return;
    }

    emit frameSaved(sequenceNo, filePath);
}

bool AcquisitionSaveWorker::writeMhaFile(const QString& filePath,
                                         const QByteArray& image,
                                         int width,
                                         int height,
                                         int bytesPerPixel,
                                         QString& errMsg) const
{
    // Store the detector frame as a self-contained 2D MetaImage file.
    if (width <= 0 || height <= 0) {
        errMsg = QString("Invalid image size for %1: %2x%3").arg(filePath).arg(width).arg(height);
        return false;
    }

    QString elementType;
    switch (bytesPerPixel) {
    case 1:
        elementType = "MET_UCHAR";
        break;
    case 2:
        elementType = "MET_USHORT";
        break;
    default:
        errMsg = QString("Unsupported bytes per pixel for %1: %2").arg(filePath).arg(bytesPerPixel);
        return false;
    }

    const qsizetype expectedSize = static_cast<qsizetype>(width) * static_cast<qsizetype>(height) * bytesPerPixel;
    if (image.size() < expectedSize) {
        errMsg = QString("Image buffer too small for %1: expected %2 bytes, got %3 bytes")
                     .arg(filePath)
                     .arg(expectedSize)
                     .arg(image.size());
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        errMsg = QString("Failed to open %1 for writing: %2").arg(filePath, file.errorString());
        return false;
    }

    QTextStream stream(&file);
    stream << "ObjectType = Image\n";
    stream << "NDims = 2\n";
    stream << "BinaryData = True\n";
    stream << "BinaryDataByteOrderMSB = False\n";
    stream << "CompressedData = False\n";
    stream << "TransformMatrix = 1 0 0 1\n";
    stream << "Offset = 0 0\n";
    stream << "CenterOfRotation = 0 0\n";
    stream << "ElementSpacing = 1 1\n";
    stream << "DimSize = " << width << ' ' << height << '\n';
    stream << "ElementType = " << elementType << '\n';
    stream << "ElementDataFile = LOCAL\n\n";
    stream.flush();

    const qint64 written = file.write(image.constData(), expectedSize);
    if (written != expectedSize) {
        errMsg = QString("Failed to write pixel data to %1. Wrote %2 of %3 bytes")
                     .arg(filePath)
                     .arg(written)
                     .arg(expectedSize);
        return false;
    }

    return true;
}
