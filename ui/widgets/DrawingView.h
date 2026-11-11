// Окно просмотра и редактирования 2D-чертежа.
// Виджет только показывает документ и передаёт действия пользователя сервисам;
// правил оформления и геометрических вычислений в нём нет.
#pragma once

#include <QWidget>
#include <optional>
#include <unordered_set>
#include <vector>

#include "AppContext.h"
#include "Tool.h"
#include "cosmocad/graphics/Scene.h"

namespace cosmocad::ui {

class DrawingView : public QWidget {
    Q_OBJECT
public:
    explicit DrawingView(app::AppContext& context, QWidget* parent = nullptr);

    void setDocument(document::Document* document);
    document::Document* document() const { return document_; }

    void setTool(Tool tool);
    Tool tool() const { return tool_; }

    // Слой для новых элементов; пустой номер — без слоя.
    void setCurrentLayer(const core::Uuid& layer) { currentLayer_ = layer; }
    const core::Uuid& currentLayer() const { return currentLayer_; }

    void fitAll();
    // Приблизить (factor > 1) или отдалить вид относительно середины окна.
    void zoomBy(double factor);
    void select(const core::Uuid& id);
    void selectAll();
    void selectMany(const std::vector<core::Uuid>& ids);  // выбрать именно эти элементы
    // Значки ограничений эскиза, которые рисуются поверх чертежа: место (координаты листа) и надпись.
    // В документ и в экспорт значки не попадают.
    struct ConstraintGlyph {
        geometry::Point2 at;
        QString text;
    };
    std::vector<ConstraintGlyph> constraintGlyphs() const;
    void clearSelection();
    void deleteSelection();
    const std::unordered_set<core::Uuid>& selection() const { return selection_; }
    const graphics::Scene& currentScene() const;

    // Ввод с клавиатуры вместо щелчка мышью:
    //   «100 50»  — точка с координатами X = 100, Y = 50 мм;
    //   «@20 0»   — точка со смещением от предыдущей;
    //   «15»      — радиус окружности или угол поворота в градусах.
    // Возвращает false, если текст не разобран или не подходит текущему инструменту.
    bool inputText(const QString& text);

    // Текст на чертеже в точке position, на текущем слое.
    bool placeText(geometry::Point2 position, const QString& text, double height);
    // Выноска со стрелкой в tip и полкой от knee, на текущем слое.
    bool placeLeader(geometry::Point2 tip, geometry::Point2 knee, const QString& text, double height);
    // Знак шероховатости: kind — 0 (способ не установлен), 1 (удалением материала), 2 (без удаления).
    bool placeRoughness(geometry::Point2 at, int kind, const QString& parameter, int quarterTurns = 0);
    // Штриховка области, ограниченной выбранными отрезками, дугами и окружностями.
    bool hatchSelection(double angleDegrees, double spacing);
    // Радиус скругления и размер фаски для инструментов «Скругление» и «Фаска», мм.
    double cornerSize() const { return cornerSize_; }

    // Преобразование координат (нужно тестам и главному окну).
    QPointF toScreen(geometry::Point2 world) const;

signals:
    void cursorMoved(const QString& text);
    void documentEdited();      // пользователь изменил документ через это окно
    void documentChanged();     // документ изменился любым способом (включая отмену)
    void selectionChanged();
    void message(const QString& text);
    void textRequested(double x, double y);  // инструмент «Текст»: нужно спросить текст и высоту
    void leaderRequested(double tipX, double tipY, double kneeX, double kneeY);  // инструмент «Выноска»
    void schematicElementRequested(double x, double y);  // инструмент «Элемент схемы»
    void schematicWireRequested(double x1, double y1, double x2, double y2);  // участок проводника
    void roughnessRequested(double x, double y);       // инструмент «Шероховатость»
    void schematicLabelRequested(double x, double y);  // инструмент «Метка цепи»

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    geometry::Point2 worldAt(const QPointF& position) const;
    geometry::Point2 snapped(const QPointF& position);
    void acceptPoint(geometry::Point2 point);
    bool acceptValue(double value);
    bool needsSelection() const;
    bool picksEntities() const;
    std::optional<core::Uuid> pick(geometry::Point2 point) const;
    core::Uuid layerForNew() const;
    std::vector<core::Uuid> selectedIds() const;
    void resetInput();
    void report(const core::Error& error);
    template <class T>
    bool finish(const core::Result<T>& result);

    app::AppContext& context_;
    document::Document* document_ = nullptr;
    core::Signal<>::Connection connection_ = 0;
    graphics::ViewTransform2D view_;
    Tool tool_ = Tool::Select;
    core::Uuid currentLayer_;
    double cornerSize_ = 5.0;
    std::vector<core::Uuid> picked_;  // элементы, указанные для текущего инструмента
    std::unordered_set<core::Uuid> selection_;
    std::vector<geometry::Point2> points_;        // точки, уже указанные для текущего инструмента
    std::optional<geometry::Point2> lastPoint_;   // для ввода «@dX dY»
    std::optional<graphics::Snap> snap_;
    geometry::Point2 cursor_;
    QPointF lastMouse_;
    QPointF pressPosition_;
    bool panning_ = false;
    bool dragging_ = false;  // выбор рамкой
    bool fitted_ = false;

    // Сцена строится заново только после изменения документа, а не при каждой перерисовке.
    mutable graphics::Scene scene_;
    mutable bool sceneDirty_ = true;
};

}  // namespace cosmocad::ui
