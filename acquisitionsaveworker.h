#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

// Background writer used by acquisition so frame persistence does not block preview updates.
class AcquisitionSaveWorker : public QObject
{
    Q_OBJECT

public:
    explicit AcquisitionSaveWorker(QObject* parent = nullptr);

public slots:
    void saveFrame(const QString& sessionDir,
                   int sequenceNo,
                   const QByteArray& image,
                   int width,
                   int height,
                   int bytesPerPixel);

signals:
    void saveFailed(const QString& message);
    void frameSaved(int sequenceNo, const QString& filePath);

private:
    // Frames are written as standalone 2D .mha files so reconstruction can consume them later.
    bool writeMhaFile(const QString& filePath,
                      const QByteArray& image,
                      int width,
                      int height,
                      int bytesPerPixel,
                      QString& errMsg) const;
};
