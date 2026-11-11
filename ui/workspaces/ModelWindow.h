// Окно 3D-модели: детали (параметры, дерево построения) и сборки (компоненты, сопряжения).
#pragma once

#include <QMainWindow>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "AppContext.h"
#include "ModelView3D.h"

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTableWidget;

namespace cosmocad::ui {

class ModelWindow : public QMainWindow {
    Q_OBJECT
public:
    // currentDrawing и drawingSelection — открытый чертёж и выбранные на нём элементы
    // (из них берётся контур для выдавливания и на него ставятся виды по модели).
    ModelWindow(app::AppContext& context, std::function<document::Document*()> currentDrawing,
                std::function<std::vector<core::Uuid>()> drawingSelection, QWidget* parent = nullptr);

    // Перечитать список документов проекта и показать текущий.
    void refresh();
    void reloadModel() { rebuild(); }  // перестроить показанную модель (после изменений, сделанных в другом окне)
    document::Document* currentDocument() const;
    void selectDocument(const core::Uuid& id);

    // Действия. Возвращают false, если действие не выполнено; причина — в строке сообщений.
    bool newPart(const QString& name);
    bool newAssembly(const QString& name);
    bool addParameter(const QString& name, const QString& expression);
    bool setParameter(const QString& name, const QString& expression);
    // Контур берётся из элементов, выбранных на чертеже; эскиз ложится на плоскость XY,
    // поднятую на planeOffset; тело вытягивается вверх на distance.
    bool extrudeContour(const QString& distance, const QString& planeOffset, bool cut);
    // Скругление или фаска на ребре между двумя гранями, выбранными в окне 3D.
    bool blendPicked(const QString& size, bool chamfer);
    bool setFeatureValue(int row, const QString& expression);
    // Место последнего щелчка по плоской грани в координатах эскиза на этой грани.
    // Пусто, если выбрана не одна плоская грань.
    std::optional<geometry::Point2> pickedPointOnFace() const;
    // Отверстие или выступ на выбранной плоской грани: окружность (circle) диаметром size1
    // или прямоугольник size1 × size2 с серединой в точке (x, y) эскиза, на глубину depth.
    // Все размеры — числа или формулы.
    bool addShapeOnPickedFace(bool circle, const QString& x, const QString& y, const QString& size1,
                              const QString& size2, const QString& depth, bool cut);
    // Размер фигуры эскиза у операции в строке row дерева: фигура shape, размер value по счёту.
    bool setSketchValue(int row, int shape, int value, const QString& expression);
    // Названия размеров эскиза операции и их формулы (для окна «Размеры эскиза»).
    QStringList sketchValueLabels(int row) const;
    QStringList sketchValues(int row) const;
    // Массив копий операции в строке row дерева: kind 0 — нет, 1 — в ряд (a, b — шаг по X и Y эскиза),
    // 2 — по кругу (a, b — центр круга, angle — угол, на котором разложить копии).
    bool setPattern(int row, int kind, const QString& count, const QString& a, const QString& b,
                    const QString& angle = QStringLiteral("360"));
    // Откат дерева: строить только до операции в строке row включительно; row < 0 — снять откат.
    bool rollTo(int row);
    // Материалы: названия из библиотеки (с поиском по части названия), назначение детали, свой материал.
    QStringList materialTitles(const QString& filter = {});
    bool setMaterial(const QString& title);
    bool addUserMaterial(const QString& name, const QString& standard, double density, const QString& note = {});
    // Протяжка контура с чертежа вдоль траектории. path — точки после начальной через «;», в каждой
    // три числа или формулы через пробел: x y z (z — вдоль нормали плоскости XY). Пример: «0 0 50; 60 0 50».
    bool sweepContour(const QString& path, const QString& bendRadius, bool cut);
    // Разнесённый вид сборки: компоненты раздвигаются на distance мм; 0 — собрать обратно.
    // Пока сборка разнесена, виды на чертёж тоже ставятся разнесёнными.
    bool explode(double distance);
    bool isExploded() const { return exploded_; }
    // Исполнения детали: запомнить нынешние значения параметров под именем; применить запомненные.
    bool saveConfiguration(const QString& name, const QString& designation = {});
    bool applyConfiguration(const QString& name);
    QStringList configurationNames();
    // Стандартное изделие из библиотеки крепежа: kind — 0 болт, 1 гайка, 2 шайба; length — только для болта.
    // В сборке изделие сразу вставляется компонентом, иначе просто появляется в проекте.
    bool addFastener(int kind, double d, double length);
    // Отрезок равнополочного уголка из библиотеки проката.
    bool addAngle(double b, double t, double length);
    // Отрезок швеллера (kind = "channel") или двутавра ("beam") по номеру профиля.
    bool addProfile(const QString& kind, const QString& name, double length);
    // Труба из полипропилена: отчёт о расчёте (пусто при ошибке) и отрезок трубы как деталь.
    QString pipeReport(const QString& material, const QString& serviceClass, double diameter, double sdr, double pressureMpa);
    bool addPipe(const QString& material, double diameter, double sdr, double length);
    // Зеркальное отражение: plane — XY, XZ или YZ; offset — сдвиг плоскости, число или формула.
    bool mirror(const QString& plane, const QString& offset);
    bool removeSelected();  // выбранная строка дерева: операция, компонент или сопряжение
    bool undo();
    bool redo();
    bool addViewToDrawing(const QString& view, double x, double y, double scale, bool showHidden);
    // Разрез модели на открытый чертёж; секущая плоскость — через середину модели.
    // at — где провести плоскость: координата модели вдоль взгляда, мм; пусто — через середину.
    // onlyCut — сечение: только то, что лежит в секущей плоскости.
    bool addSectionToDrawing(const QString& view, double x, double y, double scale, const QString& at = {},
                             bool onlyCut = false);
    // Переносит секущую плоскость разреза с такой надписью («А–А») на новую координату.
    bool moveSection(const QString& label, const QString& at);
    // Сколько свободных движений у каждого компонента сборки.
    QString freedomText();
    // Пометка «не рассекать на разрезах» для компонента в строке дерева; повтор снимает пометку.
    bool toggleWholeInSection(int row);
    bool updateDrawingViews();
    bool exportStep(const std::filesystem::path& path);
    bool exportStl(const std::filesystem::path& path);
    bool addComponent(const core::Uuid& partDocumentId);
    // Сопряжение двух граней, выбранных в окне 3D на разных компонентах.
    bool matePicked(assembly::MateKind kind, double value, bool flip);
    QString interferenceReport();
    // Зазор между двумя компонентами, на которых выбрано по грани.
    QString clearanceText();
    QString billOfMaterialsText();
    bool exportBillOfMaterials(const std::filesystem::path& path);

    ModelView3D* view() const { return view_; }
    QListWidget* tree() const { return tree_; }
    QTableWidget* parameters() const { return parameters_; }
    QLabel* info() const { return info_; }
    QLabel* messages() const { return messages_; }
    QComboBox* documentBox() const { return documents_; }

signals:
    void drawingChanged();  // на чертёж добавлены или обновлены виды
    void projectChanged();  // изменён состав проекта или документ

private slots:
    void onTreeItemChanged(QListWidgetItem* item);
    void onParameterEdited(int row, int column);
    void onPickedChanged();

private:
    void buildUi();
    void rebuild();  // перестроить текущий документ и обновить всё окно
    bool fail(const core::Error& error);
    bool fail(const QString& text);
    void done(const QString& text);

    app::AppContext& context_;
    std::function<document::Document*()> currentDrawing_;
    std::function<std::vector<core::Uuid>()> drawingSelection_;
    ModelView3D* view_ = nullptr;
    QComboBox* documents_ = nullptr;
    QListWidget* tree_ = nullptr;
    QTableWidget* parameters_ = nullptr;
    QLabel* info_ = nullptr;
    QLabel* messages_ = nullptr;
    QLabel* pickedLabel_ = nullptr;
    bool updating_ = false;
    std::vector<core::Uuid> componentOrder_;  // компонент для каждого тела в окне 3D
    bool exploded_ = false;                   // сборка показана разнесённой
};

}  // namespace cosmocad::ui
