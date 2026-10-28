#pragma once
#include "quality/qualityjob.h"
#include <QDialog>
#include <functional>
class QLabel;
class QPushButton;
class QGraphicsView;
class BoardVideoService;
class ImageQualityDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit ImageQualityDialog(BoardVideoService *board, QWidget *parent = nullptr);
    ~ImageQualityDialog() override;
    std::function<QString()> entryError;
    std::function<bool(QImage &, qint64 &)> takeRaw;
    void shutdown();
    void openResultDirectory(const QString &directory);
    void cancelSample(const QString &reason);
  signals:
    void modeChanged(bool active);

  protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

  private:
    BoardVideoService *board;
    QualityJob *job = nullptr;
    quint64 task = 0;
    bool active = false, ready = false, leaving = false, closeAfter = false;
    std::shared_ptr<const Quality::Result> result;
    QLabel *status, *psnr, *ssim, *info;
    QPushButton *enter, *capture, *leave, *save, *open, *raw, *fit;
    QGraphicsView *views[2];
    void startJob(QualityJob *next);
    void enterMode();
    void leaveMode();
    void finishLeaving();
    void updateControls();
    void showResult();
    void showInputImage();
    void fitImages();
};
