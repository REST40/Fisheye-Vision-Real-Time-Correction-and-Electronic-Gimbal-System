#pragma once
#include <QFrame>
#include <QImage>
#include <QLabel>
namespace df {
class Viewport : public QWidget {
    Q_OBJECT
  public:
    explicit Viewport(QWidget *parent = nullptr);
    void setImage(QImage value);
    void setTrackedImage(QImage value, quint64 sampleId, qint64 renderStart, qint64 renderEnd,
                         qint64 uiTake);
    void clear();
    void setSphere(bool value) {
        sphere = value;
    }
    bool interactive = false;
  signals:
    void painted(quint64 sampleId, qint64 renderStart, qint64 renderEnd, qint64 uiTake,
                 qint64 paintStart, qint64 paintEnd);
    void imageReplaced(quint64 sampleId);
    void dragged(double dx, double dy);
    void zoomed(int delta);
    void resetRequested();

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    QImage image;
    quint64 trackedId = 0;
    bool reported = false;
    qint64 trackedRenderStart = 0, trackedRenderEnd = 0, trackedTake = 0;
    bool sphere = false, dragging = false;
    QPointF position;
};
class ViewPanel : public QFrame {
  public:
    explicit ViewPanel(const QString &title, const QString &caption, QWidget *parent = nullptr);
    Viewport *viewport;
    QLabel *sizeLabel;
    QLabel *caption;
    QLabel *timestamp;
};
} // namespace df
