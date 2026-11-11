// Панель свойств: показывает свойства выбранного элемента и позволяет их изменить.
// Сама ничего не меняет в документе — передаёт правки сервису чертежа,
// поэтому каждая правка проверяется и отменяется как любое другое действие.
#pragma once

#include <QWidget>

#include "AppContext.h"
#include "DrawingView.h"

class QFormLayout;
class QVBoxLayout;

namespace cosmocad::ui {

class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    PropertiesPanel(app::AppContext& context, DrawingView* view, QWidget* parent = nullptr);

public slots:
    // Перестраивает панель под текущий выбор. Вызывается при смене выбора и изменении документа.
    void refresh();

signals:
    void message(const QString& text);

private:
    void rebuild();
    void apply(const drawing::EntityEdit& edit);

    app::AppContext& context_;
    DrawingView* view_;
    QVBoxLayout* layout_;
    QWidget* content_ = nullptr;
    core::Uuid shown_;         // элемент, для которого построена панель
    bool refreshQueued_ = false;
};

}  // namespace cosmocad::ui
