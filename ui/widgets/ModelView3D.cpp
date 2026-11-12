#include "ModelView3D.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>

namespace cosmocad::ui {

namespace {
// Цвета тел по очереди — чтобы компоненты сборки различались.
const std::uint32_t kPalette[] = {0xFFB8C4D0, 0xFFD9B38C, 0xFFA9CBA4, 0xFFC9A9D6, 0xFFE0D08A, 0xFF9FC5D8};
}  // namespace

ModelView3D::ModelView3D(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 180);
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(tr("Левая кнопка — вращать, правая или средняя — сдвигать, колесо — масштаб.\n"
                  "Щелчок по грани выбирает её (до двух граней)."));
}

std::vector<graphics::Body3D> ModelView3D::bodies() const {
    std::vector<graphics::Body3D> out;
    for (std::size_t i = 0; i < meshes_.size(); ++i) {
        graphics::Body3D body;
        body.mesh = &meshes_[i];
        body.color = kPalette[i % (sizeof kPalette / sizeof kPalette[0])];
        for (const auto& face : picked_)
            if (face.body == static_cast<int>(i)) body.highlightedFaces.push_back(face.name);
        out.push_back(std::move(body));
    }
    return out;
}

void ModelView3D::setBodies(std::vector<geometry::Mesh> meshes) {
    meshes_ = std::move(meshes);
    // Выбранные грани, которых больше нет, сбрасываются.
    const auto before = picked_.size();
    picked_.erase(std::remove_if(picked_.begin(), picked_.end(),
                                 [&](const PickedFace& face) {
                                     if (face.body < 0 || static_cast<std::size_t>(face.body) >= meshes_.size()) return true;
                                     const auto& names = meshes_[static_cast<std::size_t>(face.body)].faceNames;
                                     return std::find(names.begin(), names.end(), face.name) == names.end();
                                 }),
                  picked_.end());
    if (!fitted_ && !meshes_.empty()) {
        camera_ = graphics::fitCamera(bodies(), width(), height(), -60, 30);
        fitted_ = true;
    }
    if (meshes_.empty()) {
        fitted_ = false;
        userMoved_ = false;
    }
    dirty_ = true;
    update();
    if (before != picked_.size()) emit pickedChanged();
}

void ModelView3D::fit(double yawDegrees, double pitchDegrees) {
    camera_ = graphics::fitCamera(bodies(), width(), height(), yawDegrees, pitchDegrees);
    fitted_ = !meshes_.empty();
    userMoved_ = false;
    dirty_ = true;
    update();
}

void ModelView3D::clearPicked() {
    if (picked_.empty()) return;
    picked_.clear();
    dirty_ = true;
    update();
    emit pickedChanged();
}

void ModelView3D::ensureRendered() {
    if (!dirty_ && rendered_.width == width() && rendered_.height == height()) return;
    rendered_ = graphics::render(bodies(), camera_, width(), height());
    image_ = QImage(reinterpret_cast<const uchar*>(rendered_.pixels.data()), rendered_.width, rendered_.height,
                    rendered_.width * 4, QImage::Format_ARGB32);
    dirty_ = false;
}

QImage ModelView3D::currentImage() {
    ensureRendered();
    return image_.copy();
}

graphics::Pick3D ModelView3D::faceAt(const QPoint& position) {
    ensureRendered();
    return rendered_.pick(bodies(), position.x(), position.y());
}

std::optional<geometry::Point3> ModelView3D::pointOnPlane(const QPoint& position, const geometry::Point3& planePoint,
                                                          const geometry::Vector3& planeNormal) const {
    const auto r = camera_.right(), u = camera_.up(), e = camera_.toEye();
    const double dx = (position.x() + 0.5 - width() / 2.0) / camera_.zoom;
    const double dy = (height() / 2.0 - position.y() - 0.5) / camera_.zoom;
    // Точка на плоскости экрана, через которую идёт луч зрения (проекция параллельная).
    const geometry::Point3 start{camera_.target.x + r.x * dx + u.x * dy, camera_.target.y + r.y * dx + u.y * dy,
                                 camera_.target.z + r.z * dx + u.z * dy};
    const double along = -geometry::dotProduct(e, planeNormal);
    if (std::abs(along) < 1e-6) return std::nullopt;
    const geometry::Vector3 toPlane{planePoint.x - start.x, planePoint.y - start.y, planePoint.z - start.z};
    const double t = geometry::dotProduct(toPlane, planeNormal) / along;
    return geometry::Point3{start.x - e.x * t, start.y - e.y * t, start.z - e.z * t};
}

void ModelView3D::paintEvent(QPaintEvent*) {
    ensureRendered();
    QPainter painter(this);
    painter.drawImage(0, 0, image_);
    if (meshes_.empty()) {
        painter.setPen(QColor(0x60, 0x68, 0x70));
        painter.drawText(rect(), Qt::AlignCenter, tr("Тела нет. Добавьте операцию построения."));
    }
}

void ModelView3D::resizeEvent(QResizeEvent*) {
    // Пока пользователь не трогал вид, тело остаётся вписанным в окно при изменении его размера.
    if (!userMoved_ && !meshes_.empty()) camera_ = graphics::fitCamera(bodies(), width(), height(), camera_.yawDegrees, camera_.pitchDegrees);
    dirty_ = true;
}

void ModelView3D::mousePressEvent(QMouseEvent* event) {
    pressPosition_ = lastPosition_ = event->pos();
    moved_ = false;
}

void ModelView3D::mouseMoveEvent(QMouseEvent* event) {
    const QPoint delta = event->pos() - lastPosition_;
    if ((event->pos() - pressPosition_).manhattanLength() > 3) moved_ = true;
    if (!moved_) return;
    lastPosition_ = event->pos();
    userMoved_ = true;
    if (event->buttons() & Qt::LeftButton) {
        camera_.yawDegrees -= delta.x() * 0.5;
        camera_.pitchDegrees = std::clamp(camera_.pitchDegrees + delta.y() * 0.5, -90.0, 90.0);
    } else if (event->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        const auto r = camera_.right(), u = camera_.up();
        const double dx = -delta.x() / camera_.zoom, dy = delta.y() / camera_.zoom;
        camera_.target = {camera_.target.x + r.x * dx + u.x * dy, camera_.target.y + r.y * dx + u.y * dy,
                          camera_.target.z + r.z * dx + u.z * dy};
    } else {
        return;
    }
    dirty_ = true;
    update();
}

void ModelView3D::mouseReleaseEvent(QMouseEvent* event) {
    if (moved_ || event->button() != Qt::LeftButton) return;
    ensureRendered();
    const graphics::Pick3D pick = rendered_.pick(bodies(), event->pos().x(), event->pos().y());
    if (pick.body < 0 || pick.faceName.empty()) {
        clearPicked();
        return;
    }
    const auto same = std::find_if(picked_.begin(), picked_.end(), [&](const PickedFace& face) {
        return face.body == pick.body && face.name == pick.faceName;
    });
    if (same != picked_.end()) {
        picked_.erase(same);  // повторный щелчок снимает выбор
    } else {
        if (picked_.size() >= 2) picked_.erase(picked_.begin());
        picked_.push_back({pick.body, pick.faceName, event->pos()});
    }
    dirty_ = true;
    update();
    emit pickedChanged();
}

void ModelView3D::wheelEvent(QWheelEvent* event) {
    const double factor = std::pow(1.0015, event->angleDelta().y());
    camera_.zoom = std::clamp(camera_.zoom * factor, 0.01, 2000.0);
    userMoved_ = true;
    dirty_ = true;
    update();
}

}  // namespace cosmocad::ui
