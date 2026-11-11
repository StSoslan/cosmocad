// Окно показа 3D-тел: вращение, сдвиг, масштаб мышью и выбор граней щелчком.
#pragma once

#include <QImage>
#include <QWidget>
#include <optional>
#include <string>
#include <vector>

#include "cosmocad/graphics/Render3D.h"

namespace cosmocad::ui {

class ModelView3D : public QWidget {
    Q_OBJECT
public:
    struct PickedFace {
        int body = -1;
        std::string name;
        QPoint pixel;  // куда щёлкнули, выбирая грань
    };

    explicit ModelView3D(QWidget* parent = nullptr);

    // Новые тела. Камера сохраняется, если тела уже были показаны; иначе подбирается заново.
    void setBodies(std::vector<geometry::Mesh> meshes);
    std::size_t bodyCount() const { return meshes_.size(); }
    // Показать всё под заданными углами (спереди: -90, 0; сверху: -90, 90; изометрия: -60, 30).
    void fit(double yawDegrees, double pitchDegrees);
    const graphics::Camera3D& camera() const { return camera_; }

    const std::vector<PickedFace>& pickedFaces() const { return picked_; }
    void clearPicked();
    QImage currentImage();
    // Что под точкой окна (для подсказок и автотестов).
    graphics::Pick3D faceAt(const QPoint& position);
    // Точка плоскости, которая видна под точкой окна (луч зрения пересекается с плоскостью).
    // Пусто, если плоскость видна «с ребра».
    std::optional<geometry::Point3> pointOnPlane(const QPoint& position, const geometry::Point3& planePoint,
                                                 const geometry::Vector3& planeNormal) const;

signals:
    void pickedChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    std::vector<graphics::Body3D> bodies() const;
    void ensureRendered();

    std::vector<geometry::Mesh> meshes_;
    graphics::Camera3D camera_;
    graphics::Image3D rendered_;
    QImage image_;
    bool dirty_ = true;
    bool fitted_ = false;
    bool userMoved_ = false;
    std::vector<PickedFace> picked_;
    QPoint pressPosition_, lastPosition_;
    bool moved_ = false;
};

}  // namespace cosmocad::ui
