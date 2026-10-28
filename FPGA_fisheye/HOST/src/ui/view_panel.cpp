#include "view_panel.h"
#include "fpga/network/latencytracker.h"
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
namespace df {
Viewport::Viewport(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
void Viewport::setImage(QImage value) {
    if (trackedId && !reported)
        emit imageReplaced(trackedId);
    trackedId = 0;
    reported = false;
    image = std::move(value);
    update();
}
void Viewport::setTrackedImage(QImage value, quint64 id, qint64 start, qint64 end, qint64 take) {
    if (trackedId != id) {
        if (trackedId && !reported)
            emit imageReplaced(trackedId);
        reported = false;
    }
    trackedId = id;
    trackedRenderStart = start;
    trackedRenderEnd = end;
    trackedTake = take;
    image = std::move(value);
    update();
}
void Viewport::clear() {
    if (trackedId && !reported)
        emit imageReplaced(trackedId);
    trackedId = 0;
    reported = false;
    image = QImage();
    update();
}
void Viewport::paintEvent(QPaintEvent *) {
    const auto paintStart = LatencyTracker::nowUs();
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#090a0b"));
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (image.isNull()) {
        painter.setPen(QColor("#818a96"));
        painter.drawText(rect(), Qt::AlignCenter,
                         interactive ? "等待实时画面\n拖动调整视野" : "等待输入画面");
        return;
    }
    QRect source = image.rect();
    if (sphere) {
        double f = std::min(image.width(), image.height()) / (2 * tan(3.14159265358979323846 / 12));
        int d = std::min(std::min(image.width(), image.height()),
                         int(ceil(2 * f / sqrt(4.5 * 4.5 - 1))) + 2);
        source = QRect((image.width() - d) / 2, (image.height() - d) / 2, d, d);
    }
    QSize size = source.size();
    size.scale(this->size(), Qt::KeepAspectRatio);
    QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    painter.drawImage(target, image, source);
    painter.end();
    if (trackedId && !reported) {
        reported = true;
        emit painted(trackedId, trackedRenderStart, trackedRenderEnd, trackedTake, paintStart,
                     LatencyTracker::nowUs());
    }
}
void Viewport::mousePressEvent(QMouseEvent *e) {
    if (interactive && e->button() == Qt::LeftButton) {
        dragging = true;
        position = e->position();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
    }
}
void Viewport::mouseMoveEvent(QMouseEvent *e) {
    if (interactive && dragging) {
        auto delta = e->position() - position;
        position = e->position();
        emit dragged(delta.x(), delta.y());
        e->accept();
    }
}
void Viewport::mouseReleaseEvent(QMouseEvent *e) {
    dragging = false;
    setCursor(interactive ? Qt::OpenHandCursor : Qt::ArrowCursor);
    e->accept();
}
void Viewport::wheelEvent(QWheelEvent *e) {
    if (interactive) {
        emit zoomed(e->angleDelta().y());
        e->accept();
    }
}
void Viewport::mouseDoubleClickEvent(QMouseEvent *e) {
    if (interactive) {
        dragging = false;
        emit resetRequested();
        e->accept();
    }
}
ViewPanel::ViewPanel(const QString &title, const QString &subtitle, QWidget *parent)
    : QFrame(parent) {
    setObjectName("viewPanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    auto *heading = new QHBoxLayout;
    heading->setContentsMargins(12, 9, 12, 8);
    auto *name = new QLabel(title);
    name->setObjectName("section");
    heading->addWidget(name);
    heading->addStretch();
    sizeLabel = new QLabel("等待画面");
    sizeLabel->setObjectName("pill");
    heading->addWidget(sizeLabel);
    layout->addLayout(heading);
    viewport = new Viewport;
    layout->addWidget(viewport, 1);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(12, 8, 12, 9);
    caption = new QLabel(subtitle);
    caption->setObjectName("muted");
    footer->addWidget(caption);
    footer->addStretch();
    timestamp = new QLabel("—");
    timestamp->setObjectName("muted");
    footer->addWidget(timestamp);
    layout->addLayout(footer);
}
} // namespace df
