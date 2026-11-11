// Тесты окна: действия мышью и клавиатурой на настоящих виджетах (без экрана — платформа offscreen).
// Закрывают пробел из плана тестирования: «действия мышью в окне не проверяются автоматически».
#include <cstdlib>
#include <filesystem>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QPrinter>
#include <QListWidget>
#include <QLabel>
#include <QTableWidget>
#include <QTabBar>
#include <QToolButton>
#include <QtTest>

#include "AppContext.h"
#include "MainWindow.h"
#include "ModelWindow.h"
#include "Ribbon.h"
#include "ScenePainter.h"

using namespace cosmocad;
using geometry::Point2;

// Свои материалы в тестах лежат во временной папке, а не в настоящей папке пользователя.
static const bool kUserDirectoryIsSet = [] {
    const auto directory = std::filesystem::temp_directory_path() / "cosmocad_test_user_ui";
    std::filesystem::remove_all(directory);
    setenv("COSMOCAD_USER_DIR", directory.string().c_str(), 1);
    return true;
}();

class UiTest : public QObject {
    Q_OBJECT

    std::unique_ptr<app::AppContext> context_;
    std::unique_ptr<ui::MainWindow> window_;

    ui::DrawingView* view() { return window_->view(); }
    document::Document& doc() { return *view()->document(); }
    QPoint at(Point2 world) { return view()->toScreen(world).toPoint(); }
    void click(Point2 world, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mouseClick(view(), Qt::LeftButton, modifiers, at(world));
    }
    template <class T>
    std::vector<const T*> all() { return doc().objectsOf<T>(); }
    void markSaved() {
        for (const auto& document : context_->documents().project().documents()) document->markSaved();
    }

    // ---- Окно 3D ----
    ui::ModelWindow* model() { return window_->modelWindow(); }
    void showModel() {
        model()->show();
        QVERIFY(QTest::qWaitForWindowExposed(model()));
        QCoreApplication::processEvents();
    }
    document::Document* documentNamed(const std::string& name) {
        for (const auto& d : context_->documents().project().documents())
            if (d->name == name) return d.get();
        return nullptr;
    }
    // Точка окна 3D, под которой видна грань с таким именем (на теле body); (-1, -1), если её не видно.
    QPoint pixelOfFace(const std::string& name, int body = -1) {
        // Берём точку, вокруг которой та же грань: щелчок не должен попасть на соседнюю.
        for (int y = 6; y < model()->view()->height() - 6; y += 3)
            for (int x = 6; x < model()->view()->width() - 6; x += 3) {
                bool all = true;
                for (const QPoint d : {QPoint(0, 0), QPoint(3, 0), QPoint(-3, 0), QPoint(0, 3), QPoint(0, -3)}) {
                    const auto pick = model()->view()->faceAt(QPoint(x, y) + d);
                    all = all && pick.faceName == name && (body < 0 || pick.body == body);
                }
                if (all) return {x, y};
            }
        return {-1, -1};
    }
    bool pickFace(const std::string& name, int body = -1) {
        const QPoint p = pixelOfFace(name, body);
        if (p.x() < 0) return false;
        QTest::mouseClick(model()->view(), Qt::LeftButton, Qt::NoModifier, p);
        return true;
    }
    double volumeOf(document::Document& part) { return context_->modeling().mass(part).value().geometry.volume; }

private slots:
    void init() {
        context_ = app::AppContext::create(COSMOCAD_TEST_RESOURCES).take();
        window_ = std::make_unique<ui::MainWindow>(*context_);
        window_->newEmptyDrawing();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
        view()->fitAll();
        QCoreApplication::processEvents();
    }

    void cleanup() {
        markSaved();  // чтобы окно не спрашивало о сохранении
        window_.reset();
        context_.reset();
    }

    void newDrawingHasSheetAndNoViolationsExceptEmptyTitleBlock() {
        QCOMPARE(all<drawing::Sheet>().size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(doc().profileId), QStringLiteral("eskd"));
        // Пустой лист: не заполнены три обязательных поля основной надписи.
        QCOMPARE(window_->runCheck(), 3);
    }

    void lineIsDrawnWithTwoClicksAndUndone() {
        view()->setTool(ui::Tool::Line);
        click({50, 150});
        click({120, 150});
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(1));
        const auto* line = all<drawing::LineEntity>().front();
        // Точность щелчка ограничена одной точкой экрана.
        QVERIFY(geometry::distance(line->start, {50, 150}) < 1.0);
        QVERIFY(geometry::distance(line->end, {120, 150}) < 1.0);
        QVERIFY(doc().undo());
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(0));
    }

    void clickNearEndpointSnapsExactly() {
        context_->drawing().addLine(doc(), {50, 150}, {120, 150}).value();
        view()->setTool(ui::Tool::Line);
        click({120.4, 150.3});  // рядом с концом первого отрезка
        click({120, 200});
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(2));
        const auto* second = all<drawing::LineEntity>().back();
        QCOMPARE(second->start.x, 120.0);  // точное совпадение, а не «примерно»
        QCOMPARE(second->start.y, 150.0);
    }

    void keyboardInputGivesExactCoordinates() {
        view()->setTool(ui::Tool::Line);
        QVERIFY(view()->inputText("50 150"));
        QVERIFY(view()->inputText("@70,5 0"));  // смещение, десятичная запятая
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(1));
        const auto* line = all<drawing::LineEntity>().front();
        QCOMPARE(line->start.x, 50.0);
        QCOMPARE(line->end.x, 120.5);
        QCOMPARE(line->end.y, 150.0);

        view()->setTool(ui::Tool::Circle);
        QVERIFY(view()->inputText("100 200"));
        QVERIFY(view()->inputText("25"));  // радиус числом
        QCOMPARE(all<drawing::CircleEntity>().size(), std::size_t(1));
        QCOMPARE(all<drawing::CircleEntity>().front()->radius, 25.0);

        QVERIFY(!view()->inputText("абв"));
        QVERIFY(!view()->inputText("1 2 3"));
        view()->setTool(ui::Tool::Select);
        QVERIFY(!view()->inputText("10 10"));  // инструменту «Выбор» точки не нужны
    }

    void coordinateFieldInStatusBarWorks() {
        view()->setTool(ui::Tool::Rectangle);
        QTest::keyClicks(window_->coordinateInput(), "40 100");
        QTest::keyClick(window_->coordinateInput(), Qt::Key_Return);
        QTest::keyClicks(window_->coordinateInput(), "@60 30");
        QTest::keyClick(window_->coordinateInput(), Qt::Key_Return);
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(4));
        QVERIFY(window_->coordinateInput()->text().isEmpty());
        QVERIFY(doc().undo());  // прямоугольник — один шаг отмены
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(0));
    }

    void arcToolMakesCounterClockwiseArc() {
        view()->setTool(ui::Tool::Arc);
        QVERIFY(view()->inputText("100 150"));  // центр
        QVERIFY(view()->inputText("120 150"));  // начало: 0°
        QVERIFY(view()->inputText("100 170"));  // конец: 90°
        QCOMPARE(all<drawing::ArcEntity>().size(), std::size_t(1));
        const auto* arc = all<drawing::ArcEntity>().front();
        QCOMPARE(arc->radius, 20.0);
        QVERIFY(std::abs(arc->startAngle) < 1e-9);
        QVERIFY(std::abs(arc->endAngle - std::numbers::pi / 2) < 1e-9);
    }

    void clickSelectsAndWindowSelectsEnclosed() {
        auto a = context_->drawing().addLine(doc(), {50, 150}, {80, 150}).value();
        auto b = context_->drawing().addLine(doc(), {50, 170}, {80, 170}).value();
        context_->drawing().addLine(doc(), {60, 100}, {190, 100}).value();  // далеко и длинный
        view()->setTool(ui::Tool::Select);

        click({65, 150});
        QCOMPARE(view()->selection().size(), std::size_t(1));
        QVERIFY(view()->selection().count(a));
        click({65, 170}, Qt::ShiftModifier);  // Shift добавляет
        QCOMPARE(view()->selection().size(), std::size_t(2));
        click({30, 250});  // щелчок по пустому месту снимает выбор
        QCOMPARE(view()->selection().size(), std::size_t(0));

        QTest::mousePress(view(), Qt::LeftButton, Qt::NoModifier, at({40, 140}));
        QTest::mouseMove(view(), at({70, 160}));
        QTest::mouseRelease(view(), Qt::LeftButton, Qt::NoModifier, at({90, 180}));
        QCOMPARE(view()->selection().size(), std::size_t(2));
        QVERIFY(view()->selection().count(a) && view()->selection().count(b));
    }

    void moveCopyRotateMirrorWorkOnSelection() {
        auto id = context_->drawing().addLine(doc(), {50, 150}, {80, 150}).value();
        auto line = [&] { return doc().findAs<drawing::LineEntity>(id); };

        // Без выбора инструмент правки ничего не делает.
        view()->setTool(ui::Tool::Move);
        QVERIFY(view()->inputText("0 0"));
        QVERIFY(view()->inputText("10 10"));
        QCOMPARE(line()->start.x, 50.0);

        view()->select(id);
        view()->setTool(ui::Tool::Move);
        QVERIFY(view()->inputText("50 150"));
        QVERIFY(view()->inputText("@10 20"));
        QCOMPARE(line()->start.x, 60.0);
        QCOMPARE(line()->start.y, 170.0);

        view()->setTool(ui::Tool::Copy);
        QVERIFY(view()->inputText("60 170"));
        QVERIFY(view()->inputText("60 200"));
        QVERIFY(view()->inputText("60 220"));  // вторая копия от той же базовой точки
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(3));

        view()->setTool(ui::Tool::Rotate);
        QVERIFY(view()->inputText("60 170"));  // центр — начало отрезка
        QVERIFY(view()->inputText("90"));      // угол числом, в градусах
        QVERIFY(std::abs(line()->end.x - 60.0) < 1e-9);
        QVERIFY(std::abs(line()->end.y - 200.0) < 1e-9);

        view()->setTool(ui::Tool::Mirror);
        QVERIFY(view()->inputText("100 0"));
        QVERIFY(view()->inputText("100 10"));  // ось x = 100
        QVERIFY(std::abs(line()->start.x - 140.0) < 1e-9);

        // Каждое действие — отдельный шаг отмены.
        QCOMPARE(QString::fromStdString(doc().undoName()), QStringLiteral("Отразить"));
        QVERIFY(doc().undo());
        QCOMPARE(QString::fromStdString(doc().undoName()), QStringLiteral("Повернуть"));
    }

    void deleteKeyRemovesSelectionAndUndoRestores() {
        auto id = context_->drawing().addCircle(doc(), {100, 150}, 10).value();
        view()->setTool(ui::Tool::Select);
        click({110, 150});  // точка на окружности
        QVERIFY(view()->selection().count(id));
        QTest::keyClick(view(), Qt::Key_Delete);
        QCOMPARE(all<drawing::CircleEntity>().size(), std::size_t(0));
        QCOMPARE(view()->selection().size(), std::size_t(0));
        QVERIFY(doc().undo());
        QCOMPARE(all<drawing::CircleEntity>().size(), std::size_t(1));
    }

    void undoOfCreationDropsStaleSelection() {
        auto id = context_->drawing().addCircle(doc(), {100, 150}, 10).value();
        view()->select(id);
        QVERIFY(doc().undo());  // окружность исчезла
        QCOMPARE(view()->selection().size(), std::size_t(0));
    }

    void layerPanelControlsCurrentLayerAndVisibility() {
        auto axes = context_->drawing().addLayer(doc(), "Оси", "dash_dot").value();
        QListWidget* list = window_->layerList();
        QCOMPARE(list->count(), 2);  // «без слоя» и «Оси»
        QVERIFY(list->item(1)->text().contains(QStringLiteral("Оси")));
        QVERIFY(list->item(1)->text().contains(QStringLiteral("Штрихпунктирная")));  // название из профиля

        list->setCurrentRow(1);
        QCOMPARE(view()->currentLayer(), axes);
        view()->setTool(ui::Tool::Line);
        QVERIFY(view()->inputText("50 150"));
        QVERIFY(view()->inputText("90 150"));
        QCOMPARE(all<drawing::LineEntity>().front()->layerId, axes);  // новый элемент — на выбранном слое
        QCOMPARE(view()->currentLayer(), axes);                       // выбор слоя не сбрасывается

        list->item(1)->setCheckState(Qt::Unchecked);  // снять галочку — скрыть слой
        QVERIFY(!doc().findAs<drawing::Layer>(axes)->visible);
        std::size_t drawn = 0;
        for (const auto& item : view()->currentScene().items)
            if (!item.source.isNil()) ++drawn;
        QCOMPARE(drawn, std::size_t(0));
        QVERIFY(doc().undo());  // скрытие слоя отменяется
        QVERIFY(doc().findAs<drawing::Layer>(axes)->visible);
        QCOMPARE(window_->layerList()->item(1)->checkState(), Qt::Checked);
    }

    void checkPanelFindsLineOutsideFrame() {
        QVERIFY(context_->drawing()
                    .updateSheet(doc(), "A4", false, 0.8,
                                 {{"designation", "А.001"}, {"title", "Деталь"}, {"developer", "Иванов"}})
                    .ok());
        QCOMPARE(window_->runCheck(), 0);
        view()->setTool(ui::Tool::Line);
        QVERIFY(view()->inputText("100 100"));
        QVERIFY(view()->inputText("400 100"));  // за рамкой листа А4
        QCOMPARE(window_->runCheck(), 1);
    }

    void textToolAsksForTextAndPlacesItOnCurrentLayer() {
        auto layer = context_->drawing().addLayer(doc(), "Надписи", "solid_thin").value();
        window_->layerList()->setCurrentRow(1);
        // В тесте окно ввода не показываем: отключаем его и ловим сам запрос.
        QVERIFY(QObject::disconnect(view(), &ui::DrawingView::textRequested, window_.get(), nullptr));
        QSignalSpy spy(view(), &ui::DrawingView::textRequested);
        view()->setTool(ui::Tool::Text);
        QVERIFY(view()->inputText("60 200"));
        QCOMPARE(spy.count(), 1);  // окно должно спросить текст
        QCOMPARE(spy.at(0).at(0).toDouble(), 60.0);
        QCOMPARE(spy.at(0).at(1).toDouble(), 200.0);
        // Ответ пользователя из окна ввода:
        QVERIFY(view()->placeText({60, 200}, QStringLiteral("Неуказанные радиусы 2 мм"), 3.5));
        QCOMPARE(all<drawing::TextEntity>().size(), std::size_t(1));
        QCOMPARE(all<drawing::TextEntity>().front()->layerId, layer);
        QVERIFY(!view()->placeText({60, 190}, QString(), 3.5));  // пустой текст не создаётся
        QCOMPARE(all<drawing::TextEntity>().size(), std::size_t(1));
    }

    void radiusAndDiameterToolsPickCircleThenDirection() {
        context_->drawing().addCircle(doc(), {100, 150}, 20).value();
        view()->setTool(ui::Tool::Radius);
        QVERIFY(view()->inputText("120 150"));  // щелчок по окружности
        QVERIFY(view()->inputText("100 190"));  // направление стрелки — вверх
        QCOMPARE(all<drawing::RadialDimension>().size(), std::size_t(1));
        const auto* r = all<drawing::RadialDimension>().front();
        QCOMPARE(r->radius, 20.0);
        QCOMPARE(r->center.x, 100.0);
        QVERIFY(std::abs(r->angle - std::numbers::pi / 2) < 1e-9);
        QVERIFY(!r->diameter);

        view()->setTool(ui::Tool::Diameter);
        click({100, 130});  // мышью: точка на окружности снизу
        click({130, 180});
        QCOMPARE(all<drawing::RadialDimension>().size(), std::size_t(2));
        QVERIFY(all<drawing::RadialDimension>().back()->diameter);

        view()->setTool(ui::Tool::Radius);
        QVERIFY(view()->inputText("30 250"));  // мимо окружности — ничего не создаётся
        QVERIFY(view()->inputText("40 250"));
        QCOMPARE(all<drawing::RadialDimension>().size(), std::size_t(2));
    }

    void angleToolMeasuresCounterClockwise() {
        view()->setTool(ui::Tool::Angle);
        QVERIFY(view()->inputText("50 100"));    // вершина
        QVERIFY(view()->inputText("@30 0"));     // первая сторона — вправо
        QVERIFY(view()->inputText("50 130"));    // вторая сторона — вверх
        QVERIFY(view()->inputText("70 120"));    // место размерной дуги
        QCOMPARE(all<drawing::AngularDimension>().size(), std::size_t(1));
        const auto* a = all<drawing::AngularDimension>().front();
        QVERIFY(std::abs(a->valueDegrees() - 90.0) < 1e-9);
        QVERIFY(std::abs(a->arcRadius - std::hypot(20.0, 20.0)) < 1e-9);
    }

    void trimToolRemovesClickedPart() {
        auto target = context_->drawing().addLine(doc(), {40, 150}, {160, 150}).value();
        context_->drawing().addLine(doc(), {80, 130}, {80, 170}).value();
        context_->drawing().addLine(doc(), {120, 130}, {120, 170}).value();
        view()->setTool(ui::Tool::Trim);
        click({100, 150});  // середина между вертикалями
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(4));
        QCOMPARE(doc().findAs<drawing::LineEntity>(target)->end.x, 80.0);
        click({100, 200});  // по пустому месту — ничего не происходит
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(4));
        QVERIFY(doc().undo());
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(3));
    }

    void filletAndChamferUseTypedSize() {
        auto a = context_->drawing().addLine(doc(), {50, 100}, {150, 100}).value();
        auto b = context_->drawing().addLine(doc(), {50, 100}, {50, 180}).value();
        view()->setTool(ui::Tool::Fillet);
        QCOMPARE(view()->cornerSize(), 5.0);
        QVERIFY(view()->inputText("12"));  // радиус числом до щелчков
        QCOMPARE(view()->cornerSize(), 12.0);
        click({120, 100});
        click({50, 150});
        QCOMPARE(all<drawing::ArcEntity>().size(), std::size_t(1));
        QCOMPARE(all<drawing::ArcEntity>().front()->radius, 12.0);
        QVERIFY(std::abs(doc().findAs<drawing::LineEntity>(a)->start.x - 62.0) < 1e-9);
        QVERIFY(std::abs(doc().findAs<drawing::LineEntity>(b)->start.y - 112.0) < 1e-9);

        QVERIFY(doc().undo());
        view()->setTool(ui::Tool::Chamfer);
        QVERIFY(view()->inputText("8"));
        click({120, 100});
        click({50, 150});
        QCOMPARE(all<drawing::ArcEntity>().size(), std::size_t(0));
        QCOMPARE(all<drawing::LineEntity>().size(), std::size_t(3));
        QVERIFY(std::abs(doc().findAs<drawing::LineEntity>(a)->start.x - 58.0) < 1e-9);
    }

    void hatchSelectionNeedsClosedContour() {
        context_->drawing().addRectangle(doc(), {60, 120}, {140, 180}).value();
        context_->drawing().addCircle(doc(), {100, 150}, 15).value();
        QVERIFY(!view()->hatchSelection(45, 3));  // ничего не выбрано
        view()->selectAll();
        QVERIFY(view()->hatchSelection(45, 3));
        QCOMPARE(all<drawing::Hatch>().size(), std::size_t(1));
        QCOMPARE(all<drawing::Hatch>().front()->loops.size(), std::size_t(2));  // контур и отверстие

        // Незамкнутый контур: сообщение, штриховки нет.
        auto open = context_->drawing().addLine(doc(), {30, 250}, {60, 250}).value();
        view()->select(open);
        QSignalSpy spy(view(), &ui::DrawingView::message);
        QVERIFY(!view()->hatchSelection(45, 3));
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toString().contains(QStringLiteral("не замкнут")));
        QCOMPARE(all<drawing::Hatch>().size(), std::size_t(1));
    }

    void leaderToolAsksForTextAfterTwoPoints() {
        QVERIFY(QObject::disconnect(view(), &ui::DrawingView::leaderRequested, window_.get(), nullptr));
        QSignalSpy spy(view(), &ui::DrawingView::leaderRequested);
        view()->setTool(ui::Tool::Leader);
        QVERIFY(view()->inputText("60 150"));
        QCOMPARE(spy.count(), 0);
        QVERIFY(view()->inputText("90 180"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(view()->placeLeader({60, 150}, {90, 180}, QStringLiteral("Фаска"), 3.5));
        QCOMPARE(all<drawing::Leader>().size(), std::size_t(1));
    }

    // Панель свойств: правка уже созданных элементов настоящими полями ввода.
    void propertiesPanelEditsTextAndRadius() {
        auto text = context_->drawing().addText(doc(), {60, 200}, "Черновик", 3.5).value();
        auto circle = context_->drawing().addCircle(doc(), {100, 150}, 10).value();
        ui::PropertiesPanel* panel = window_->properties();

        QCoreApplication::processEvents();
        QVERIFY(!panel->findChild<QLineEdit*>("prop_text"));  // ничего не выбрано — полей нет

        view()->select(text);
        QCoreApplication::processEvents();
        auto* edit = panel->findChild<QLineEdit*>("prop_text");
        QVERIFY(edit);
        QCOMPARE(edit->text(), QStringLiteral("Черновик"));
        edit->setText(QStringLiteral("Чистовик"));
        emit edit->editingFinished();
        QCOMPARE(QString::fromStdString(doc().findAs<drawing::TextEntity>(text)->text), QStringLiteral("Чистовик"));

        QCoreApplication::processEvents();  // панель перестроилась под новое состояние
        auto* height = panel->findChild<QComboBox*>("prop_height");
        QVERIFY(height);
        int five = height->findData(5.0);
        QVERIFY(five >= 0);
        height->setCurrentIndex(five);
        emit height->activated(five);
        QCOMPARE(doc().findAs<drawing::TextEntity>(text)->height, 5.0);

        // Отмена возвращает и текст, и высоту; панель показывает действующие значения.
        QVERIFY(doc().undo());
        QVERIFY(doc().undo());
        QCoreApplication::processEvents();
        QCOMPARE(panel->findChild<QLineEdit*>("prop_text")->text(), QStringLiteral("Черновик"));

        view()->select(circle);
        QCoreApplication::processEvents();
        auto* radius = panel->findChild<QDoubleSpinBox*>("prop_radius");
        QVERIFY(radius);
        QCOMPARE(radius->value(), 10.0);
        radius->setValue(17.5);
        QCOMPARE(doc().findAs<drawing::CircleEntity>(circle)->radius, 17.5);
        QVERIFY(!panel->findChild<QLineEdit*>("prop_text"));  // у окружности текста нет
    }

    void propertiesPanelSetsToleranceAndLayer() {
        auto layer = context_->drawing().addLayer(doc(), "Размеры", "solid_thin").value();
        auto dim = context_->drawing().addLinearDimension(doc(), {60, 150}, {140, 150}, -12).value();
        view()->select(dim);
        QCoreApplication::processEvents();
        ui::PropertiesPanel* panel = window_->properties();

        auto* suffix = panel->findChild<QLineEdit*>("prop_suffix");
        QVERIFY(suffix);
        suffix->setText(QStringLiteral("H7"));
        emit suffix->editingFinished();
        QCOMPARE(doc().findAs<drawing::LinearDimension>(dim)->note.suffix, std::string("H7"));

        QCoreApplication::processEvents();
        auto* upper = panel->findChild<QLineEdit*>("prop_upper");
        QVERIFY(upper);
        upper->setText(QStringLiteral("+0,2"));
        emit upper->editingFinished();
        QCOMPARE(doc().findAs<drawing::LinearDimension>(dim)->note.upper, std::string("+0,2"));
        QCOMPARE(doc().findAs<drawing::LinearDimension>(dim)->note.suffix, std::string("H7"));  // прежнее не потеряно

        QCoreApplication::processEvents();
        auto* layerBox = panel->findChild<QComboBox*>("prop_layer");
        QVERIFY(layerBox);
        int index = layerBox->findText(QStringLiteral("Размеры"));
        QVERIFY(index > 0);
        layerBox->setCurrentIndex(index);
        emit layerBox->activated(index);
        QCOMPARE(doc().findAs<drawing::LinearDimension>(dim)->layerId, layer);
    }

    void printingProducesOnePageAtSheetSize() {
        markSaved();
        window_->newProjectWithDemo();
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(QStringLiteral("ui_print.pdf"));
        printer.setFullPage(true);
        printer.setPageSize(QPageSize(QPageSize::A4));
        QVERIFY(ui::printScene(view()->currentScene(), printer));
        QFileInfo info(QStringLiteral("ui_print.pdf"));
        QVERIFY(info.exists());
        QVERIFY(info.size() > 2000);
    }

    // Чертёж-образец со всеми новыми элементами: сохраняется картинкой и PDF для просмотра глазами
    // и проходит проверку оформления без замечаний.
    void sampleSectionDrawingPassesCheckAndRenders() {
        auto& service = context_->drawing();
        QVERIFY(service.updateSheet(doc(), "A4", false, 0.8,
                                    {{"designation", "КСМ.002.001"}, {"title", "Втулка"}, {"developer", "Иванов"},
                                     {"scale", "1:1"}, {"sheetCount", "1"}}).ok());
        auto contour = service.addLayer(doc(), "Контур", "solid_main").value();
        auto thin = service.addLayer(doc(), "Тонкие", "solid_thin").value();
        auto axes = service.addLayer(doc(), "Оси", "dash_dot").value();

        // Разрез втулки: наружный прямоугольник 80 × 50 с отверстием-окружностью, фаской и скруглением.
        auto rect = service.addRectangle(doc(), {60, 170}, {140, 220}, contour).value();
        auto hole = service.addCircle(doc(), {100, 195}, 12, contour).value();
        QVERIFY(service.filletLines(doc(), rect[1], {140, 200}, rect[2], {120, 220}, 8).ok());  // правый верхний угол
        QVERIFY(service.chamferLines(doc(), rect[3], {60, 200}, rect[2], {80, 220}, 6).ok());   // левый верхний угол

        view()->selectAll();
        window_->layerList()->setCurrentRow(2);  // слой «Тонкие» — для штриховки и размеров
        QCOMPARE(view()->currentLayer(), thin);
        QVERIFY(view()->hatchSelection(45, 3));
        view()->clearSelection();

        QVERIFY(service.addLine(doc(), {82, 195}, {118, 195}, axes).ok());
        QVERIFY(service.addLine(doc(), {100, 177}, {100, 213}, axes).ok());
        QVERIFY(service.addLinearDimension(doc(), {60, 170}, {140, 170}, -12, thin).ok());
        QVERIFY(service.addRadialDimension(doc(), {100, 195}, 12, 0.6, true, thin).ok());
        const auto* fillet = all<drawing::ArcEntity>().front();
        QVERIFY(service.addRadialDimension(doc(), fillet->center, fillet->radius, std::numbers::pi / 4, false, thin).ok());
        QVERIFY(service.addAngularDimension(doc(), {60, 214}, {60, 190}, {66, 220}, 14, thin).ok());
        QVERIFY(view()->placeText({60, 235}, QStringLiteral("Втулка. Разрез А–А"), 5));
        QVERIFY(view()->placeLeader({63, 217}, {45, 232}, QStringLiteral("Фаска"), 3.5));
        // Допуск на размер 80: отклонения +0,2 и −0,1.
        drawing::EntityEdit tolerance;
        tolerance.note = drawing::DimensionNote{"", "+0,2", "\u22120,1"};
        QVERIFY(service.editEntity(doc(), all<drawing::LinearDimension>().front()->id(), tolerance).ok());
        Q_UNUSED(hole);

        QCOMPARE(window_->runCheck(), 0);
        view()->fitAll();
        QCoreApplication::processEvents();
        QVERIFY(window_->grab().save(QStringLiteral("ui_sample.png")));
        QVERIFY(ui::exportPdf(view()->currentScene(), QStringLiteral("ui_sample.pdf")));
    }

    // Путь пользователя: контур на чертеже → тело → скругление ребра, выбранного щелчками по граням.
    void contourFromDrawingBecomesBodyAndPickedEdgeIsFilleted() {
        auto& service = static_cast<drawing::IDrawingService&>(context_->drawing());
        QVERIFY(service.addRectangle(doc(), {40, 150}, {120, 200}).ok());
        QVERIFY(service.addCircle(doc(), {80, 175}, 10).ok());
        view()->selectAll();
        showModel();
        QVERIFY(model()->newPart("Плита"));
        QVERIFY(model()->extrudeContour("12", "0", false));
        document::Document& part = *model()->currentDocument();
        QVERIFY(std::abs(volumeOf(part) - (80.0 * 50 - M_PI * 100) * 12) < 1e-3);
        QCOMPARE(model()->tree()->count(), 1);
        QCOMPARE(model()->view()->bodyCount(), std::size_t(1));
        QVERIFY(model()->info()->text().contains(QStringLiteral("масса")));

        // Без выбранных граней скругление не делается, и окно говорит почему.
        QVERIFY(!model()->blendPicked("2", false));
        QVERIFY(model()->messages()->text().contains(QStringLiteral("две грани")));
        // Щелчки по верхней грани и по стенке отверстия: ребро между ними — кромка отверстия.
        QVERIFY(pickFace("E1/end"));
        QVERIFY(pickFace("E1/side/2000"));
        QCOMPARE(model()->view()->pickedFaces().size(), std::size_t(2));
        const double before = volumeOf(part);
        QVERIFY(model()->blendPicked("2", false));
        QCOMPARE(model()->tree()->count(), 2);
        QVERIFY(volumeOf(part) < before);
        QVERIFY(model()->view()->pickedFaces().empty());

        // Отмена и повтор в окне модели.
        QVERIFY(model()->undo());
        QCOMPARE(model()->tree()->count(), 1);
        QVERIFY(std::abs(volumeOf(part) - before) < 1e-9);
        QVERIFY(model()->redo());
        QCOMPARE(model()->tree()->count(), 2);
        // Вырез: тот же контур отверстия пошире — через строку дерева удаляется.
        model()->tree()->setCurrentRow(1);
        QVERIFY(model()->removeSelected());
        QCOMPARE(model()->tree()->count(), 1);
        // Незамкнутый контур — понятный отказ.
        view()->clearSelection();
        QVERIFY(!model()->extrudeContour("5", "0", false));
        QVERIFY(model()->messages()->text().contains(QStringLiteral("Не выполнено")));
    }

    void editingParameterInTableRebuildsModel() {
        markSaved();
        window_->newProjectWithDemo();
        showModel();
        document::Document* support = documentNamed("Опора");
        QVERIFY(support);
        model()->selectDocument(support->id());
        QCOMPARE(model()->currentDocument(), support);
        QCOMPARE(model()->tree()->count(), 5);
        QCOMPARE(model()->parameters()->rowCount(), 6);
        const double before = volumeOf(*support);
        int row = -1;
        for (int r = 0; r < model()->parameters()->rowCount(); ++r)
            if (model()->parameters()->item(r, 0)->text() == QStringLiteral("длина")) row = r;
        QVERIFY(row >= 0);
        model()->parameters()->item(row, 1)->setText("2 * ширина");  // как при вводе в ячейку
        QVERIFY(volumeOf(*support) > before + 5000);
        QCOMPARE(model()->parameters()->item(row, 2)->text(), QStringLiteral("100,000"));
        for (const auto& status : context_->modeling().status(*support))
            QCOMPARE(status.state, modeling::FeatureState::UpToDate);
        // Ошибка в формуле: модель не меняется, в ячейку возвращается прежняя формула.
        const double good = volumeOf(*support);
        model()->parameters()->item(row, 1)->setText("нет_такого * 2");
        QCOMPARE(model()->parameters()->item(row, 1)->text(), QStringLiteral("2 * ширина"));
        QVERIFY(model()->messages()->text().contains(QStringLiteral("нет_такого")));
        QCOMPARE(volumeOf(*support), good);
    }

    void uncheckingFeatureSuppressesItAndFailureIsShown() {
        markSaved();
        window_->newProjectWithDemo();
        showModel();
        document::Document* support = documentNamed("Опора");
        model()->selectDocument(support->id());
        // Отключаем бобышку (вторая строка): отверстию, скруглению и фаске не на что опереться.
        model()->tree()->item(1)->setCheckState(Qt::Unchecked);
        QVERIFY(context_->modeling().features(*support)[1]->suppressed);
        QVERIFY(model()->tree()->item(3)->text().contains(QStringLiteral("сбой")));
        model()->tree()->item(1)->setCheckState(Qt::Checked);
        for (int r = 0; r < model()->tree()->count(); ++r)
            QVERIFY(!model()->tree()->item(r)->text().contains(QStringLiteral("сбой")));
        // Откат: деталь после первой операции — сплошная плита с крепёжными отверстиями.
        const double fullVolume = volumeOf(*support);
        QVERIFY(model()->rollTo(0));
        QVERIFY(std::abs(volumeOf(*support) - (80.0 * 50 - 4 * M_PI * 9) * 10) < 1e-3);
        QVERIFY(model()->tree()->item(2)->text().contains(QStringLiteral("ждёт")));
        QVERIFY(model()->rollTo(0));  // повторное нажатие снимает откат
        QVERIFY(std::abs(volumeOf(*support) - fullVolume) < 1e-9);
        QVERIFY(!model()->tree()->item(2)->text().contains(QStringLiteral("ждёт")));
        // Материал из библиотеки: поиск, назначение, свой материал.
        QVERIFY(model()->materialTitles(QStringLiteral("амг")).size() == 4);
        const double steelMass = context_->modeling().mass(*support).value().massGrams;
        QVERIFY(model()->setMaterial(QStringLiteral("АМг6")));
        QVERIFY(context_->modeling().mass(*support).value().massGrams < steelMass * 0.4);
        QVERIFY(!model()->setMaterial(QStringLiteral("Нет такого")));
        QVERIFY(model()->addUserMaterial(QStringLiteral("Полиамид"), QStringLiteral("ТУ 1"), 1.14, QStringLiteral("паспорт")));
        QVERIFY(model()->materialTitles(QStringLiteral("полиамид")).contains(QStringLiteral("Полиамид ТУ 1")));
        QVERIFY(!model()->addUserMaterial(QStringLiteral("Плохой"), QString(), 0.0));
        QVERIFY(context_->materials().removeUser("Полиамид ТУ 1").ok());
        QVERIFY(model()->setMaterial(QStringLiteral("Сталь 45 ГОСТ 1050-2013")));

        // Исполнения: запомнить размеры, изменить, вернуть запомненные одним действием.
        QVERIFY(model()->saveConfiguration(QStringLiteral("Базовое")));
        QVERIFY(model()->setParameter(QStringLiteral("длина"), QStringLiteral("120")));
        QVERIFY(std::abs(volumeOf(*support) - fullVolume) > 1000.0);
        QVERIFY(model()->saveConfiguration(QStringLiteral("Длинное"), QStringLiteral("КСМ.003.001-01")));
        QCOMPARE(model()->configurationNames(), QStringList({QStringLiteral("Базовое"), QStringLiteral("Длинное")}));
        QVERIFY(model()->applyConfiguration(QStringLiteral("Базовое")));
        QVERIFY(std::abs(volumeOf(*support) - fullVolume) < 1e-6);
        QVERIFY(!model()->applyConfiguration(QStringLiteral("Нет такого")));

        // Зеркало относительно нижней плоскости плиты: тело удваивается; отмена возвращает прежнее.
        const int rowsBefore = model()->tree()->count();
        QVERIFY(model()->mirror("XY", "0"));
        QVERIFY(std::abs(volumeOf(*support) - 2.0 * fullVolume) < 1e-3);
        QVERIFY(model()->tree()->item(rowsBefore)->text().contains(QStringLiteral("Зеркало")));
        QVERIFY(!model()->mirror("XY", "нет_такого_параметра"));
        model()->undo();
        QVERIFY(std::abs(volumeOf(*support) - fullVolume) < 1e-9);
        QCOMPARE(model()->tree()->count(), rowsBefore);

        // Размер операции меняется по номеру строки.
        const double before = volumeOf(*support);
        QVERIFY(model()->setFeatureValue(1, "высота_бобышки + 10"));
        QVERIFY(volumeOf(*support) > before);
        QVERIFY(!model()->setFeatureValue(1, "2 +"));
    }

    void assemblyIsBuiltByPickingFaces() {
        markSaved();
        window_->newProjectWithDemo();
        showModel();
        QVERIFY(model()->newAssembly("Узел"));
        document::Document& assemblyDocument = *model()->currentDocument();
        QVERIFY(model()->addComponent(documentNamed("Опора")->id()));
        QVERIFY(model()->addComponent(documentNamed("Палец")->id()));
        QCOMPARE(model()->view()->bodyCount(), std::size_t(2));
        QCOMPARE(model()->tree()->count(), 2);

        // Соосность: стенка отверстия опоры и боковая поверхность пальца.
        model()->view()->fit(-60, 55);
        QCoreApplication::processEvents();
        QVERIFY(pickFace("E3/side/1000", 0));
        QVERIFY(pickFace("E1/side/1000", 1));
        QVERIFY(model()->matePicked(assembly::MateKind::Concentric, 0.0, false));
        const auto* pin = assemblyDocument.objectsOf<assembly::Component>().at(1);
        QVERIFY(std::abs(pin->placement.apply({0, 0, 0}).x - 40.0) < 1e-6);
        QVERIFY(std::abs(pin->placement.apply({0, 0, 0}).y - 25.0) < 1e-6);
        QCOMPARE(model()->tree()->count(), 3);

        // Зазор между компонентами: щелчок по грани на каждом. Палец пока висит над опорой:
        // его низ на высоте 60, верх бобышки на высоте 25, ближайшая точка — на фаске отверстия.
        QVERIFY(model()->clearanceText().contains(QStringLiteral("Щёлкните")));  // грани ещё не выбраны
        QVERIFY(pickFace("E1/end", 0));
        QVERIFY(pickFace("E1/end", 1));
        QVERIFY2(model()->clearanceText().contains(QStringLiteral("35,016 мм")), qPrintable(model()->clearanceText()));
        model()->view()->clearPicked();

        // Степени свободы: палец на оси отверстия может скользить вдоль неё и вращаться.
        QVERIFY2(model()->freedomText().contains(QStringLiteral("свободных движений — 2 из 6")), qPrintable(model()->freedomText()));
        QVERIFY(model()->freedomText().contains(QStringLiteral("закреплён")));

        // Пометка «не рассекать» ставится на компонент и снимается повторным нажатием; на сопряжение не ставится.
        QVERIFY(model()->toggleWholeInSection(1));
        QVERIFY(model()->tree()->item(1)->text().contains(QStringLiteral("не рассекать")));
        QVERIFY(pin->wholeInSection);
        QVERIFY(model()->toggleWholeInSection(1));
        QVERIFY(!model()->tree()->item(1)->text().contains(QStringLiteral("не рассекать")));
        QVERIFY(!model()->toggleWholeInSection(2));

        // Две грани одного компонента — отказ с объяснением.
        QVERIFY(pickFace("E1/end", 0));
        QVERIFY(!model()->matePicked(assembly::MateKind::Coincident, 0.0, false));
        QVERIFY(model()->messages()->text().contains(QStringLiteral("разных")));

        QVERIFY(model()->billOfMaterialsText().contains(QStringLiteral("Палец")));
        QVERIFY(model()->billOfMaterialsText().contains(QStringLiteral("КСМ.003.001")));

        // Разнесённый вид: палец уезжает вверх, сопряжения не меняются; 0 — собрать обратно.
        const double pinZ = pin->placement.apply({0, 0, 0}).z;
        QVERIFY(model()->explode(40));
        QVERIFY(model()->isExploded());
        QVERIFY(std::abs(pin->explode.z - 40.0) < 1e-9);
        QVERIFY(std::abs(pin->placement.apply({0, 0, 0}).z - pinZ) < 1e-9);
        QVERIFY(model()->grab().save("ui_model_exploded.png"));
        QVERIFY(model()->explode(0));
        QVERIFY(!model()->isExploded());

        // Крепёж из библиотеки вставляется в сборку и попадает в состав изделия; чего нет в стандарте — отказ.
        const int rowsWithoutBolt = model()->tree()->count();
        QVERIFY(model()->addFastener(0, 8, 30));
        QCOMPARE(model()->tree()->count(), rowsWithoutBolt + 1);
        QVERIFY(model()->billOfMaterialsText().contains(QStringLiteral("Болт М8×30 ГОСТ 7798-70")));
        QVERIFY(!model()->addFastener(0, 8, 31));
        QVERIFY(!model()->addFastener(2, 45, 0));
        QCOMPARE(model()->tree()->count(), rowsWithoutBolt + 1);

        // Труба из полипропилена: расчёт виден по-русски, отрезок вставляется в сборку; серии нет — отказ.
        const QString pipeText = model()->pipeReport(QStringLiteral("PP-R"), QStringLiteral("1"), 25, 6, 0.6);
        QVERIFY(pipeText.contains(QStringLiteral("труба подходит")));
        QVERIFY(model()->pipeReport(QStringLiteral("PP-R"), QStringLiteral("5"), 25, 11, 1.0).contains(QStringLiteral("НЕ подходит")));
        QVERIFY(model()->pipeReport(QStringLiteral("PP-R"), QStringLiteral("1"), 25, 8, 0.6).isEmpty());
        QVERIFY(model()->addPipe(QStringLiteral("PP-R"), 25, 6, 300));
        QVERIFY(model()->billOfMaterialsText().contains(QStringLiteral("Труба PP-R 25×4,2 L=300")));
        QVERIFY(!model()->addPipe(QStringLiteral("PP-R"), 26, 6, 300));
        // Уголок из библиотеки проката; размера нет в стандарте — отказ.
        QVERIFY(model()->addAngle(50, 5, 200));
        QVERIFY(model()->billOfMaterialsText().contains(QStringLiteral("Уголок 50×50×5 ГОСТ 8509-93 L=200")));
        QVERIFY(!model()->addAngle(50, 9, 200));
        QVERIFY(model()->addProfile(QStringLiteral("channel"), QStringLiteral("10П"), 200));
        QVERIFY(model()->billOfMaterialsText().contains(QStringLiteral("Швеллер 10П ГОСТ 8240-97 L=200")));
        QVERIFY(!model()->addProfile(QStringLiteral("beam"), QStringLiteral("20Б0"), 200));  // исключён изменением № 1
        model()->undo();
        model()->undo();
        model()->undo();
        QCOMPARE(model()->tree()->count(), rowsWithoutBolt + 1);
        model()->undo();
        QCOMPARE(model()->tree()->count(), rowsWithoutBolt);
        QVERIFY(!model()->interferenceReport().isEmpty());
        // Удаление компонента убирает и его сопряжение.
        model()->tree()->setCurrentRow(1);
        QVERIFY(model()->removeSelected());
        QCOMPARE(model()->tree()->count(), 1);
    }

    void modelViewGoesToDrawingAndFollowsModel() {
        markSaved();
        window_->newProjectWithDemo();
        showModel();
        document::Document* support = documentNamed("Опора");
        model()->selectDocument(support->id());
        const std::size_t linesBefore = all<drawing::LineEntity>().size();
        QVERIFY(model()->addViewToDrawing("top", 100, 120, 1.0, false));
        QCOMPARE(all<drawing::ModelView>().size(), std::size_t(1));
        QVERIFY(all<drawing::LineEntity>().size() >= linesBefore + 4);
        QVERIFY(!model()->addViewToDrawing("сбоку", 100, 120, 1.0, false));

        const auto widthOfView = [&] {
            geometry::BBox2 box;
            for (const auto& id : all<drawing::ModelView>().front()->entities)
                if (const auto* line = doc().findAs<drawing::LineEntity>(id)) {
                    box.expand(line->start);
                    box.expand(line->end);
                }
            return box.width();
        };
        QVERIFY(std::abs(widthOfView() - 80.0) < 1e-6);
        QVERIFY(model()->setParameter("длина", "110"));
        // Размер длины, поставленный на вид, после обновления сам становится 110.
        auto dimension = static_cast<drawing::IDrawingService&>(context_->drawing()).addLinearDimension(doc(), {60, 95}, {140, 95}, -8.0);
        QVERIFY(dimension.ok());
        QVERIFY(std::abs(widthOfView() - 80.0) < 1e-6);  // чертёж сам не меняется
        QVERIFY(model()->updateDrawingViews());
        QVERIFY(std::abs(widthOfView() - 110.0) < 1e-6);
        QVERIFY(std::abs(doc().findAs<drawing::LinearDimension>(dimension.value())->value() - 110.0) < 1e-6);
        QVERIFY(model()->messages()->text().contains(QStringLiteral("размеров переехало за моделью: 1")));
    }

    void drawingChooserSwitchesBetweenDrawingsAndExportsWork() {
        markSaved();
        window_->newProjectWithDemo();
        QCOMPARE(window_->drawingTabs()->count(), 3);
        window_->drawingTabs()->setCurrentIndex(1);
        QCOMPARE(QString::fromStdString(doc().name), QStringLiteral("Опора. Чертёж"));
        view()->fitAll();
        QCOMPARE(window_->runCheck(), 0);
        QVERIFY(view()->grab().save("ui_model_drawing.png"));
        QVERIFY(ui::exportPdf(view()->currentScene(), "ui_model_drawing.pdf"));
        // Знаки шероховатости: на верхней поверхности бобышки и общий знак в углу листа.
        QVERIFY(view()->placeRoughness({75, 247.5}, 1, "Ra 3,2"));
        QVERIFY(!view()->placeRoughness({75, 247.5}, 0, "3,2"));  // без символа параметра — отказ
        QVERIFY(window_->setSheetRoughness("Ra 6,3", 0, true));
        QVERIFY(!window_->setSheetRoughness("шершаво", 0, false));
        QCOMPARE(doc().objectsOf<drawing::RoughnessSign>().size(), std::size_t(2));
        // Технические требования над основной надписью.
        QVERIFY(window_->setTechnicalRequirements({QStringLiteral("Неуказанные предельные отклонения размеров: H14, h14, ±IT14/2."),
                                                   QStringLiteral("Острые кромки притупить.")}));
        QVERIFY(!window_->setTechnicalRequirements({QStringLiteral(" ")}));
        // Разрез А–А: штриховка, надпись и обозначение плоскости на виде сверху; оформление без замечаний.
        showModel();
        model()->selectDocument(documentNamed("Опора")->id());
        QVERIFY(model()->addSectionToDrawing("front", 150, 115, 1.0));
        QVERIFY(!model()->addSectionToDrawing("left", 60, 100, 1.0, "сбоку"));  // положение плоскости — не число
        QVERIFY(!model()->addSectionToDrawing("left", 60, 100, 1.0, "500"));    // плоскость мимо детали
        QCOMPARE(window_->staleViewCount(), 0);
        // Секущую плоскость готового разреза можно перенести; сечение показывает только срез.
        QVERIFY(model()->moveSection(QStringLiteral("А–А"), "20"));
        QVERIFY(!model()->moveSection(QStringLiteral("Я–Я"), "20"));
        QVERIFY(!model()->moveSection(QStringLiteral("А–А"), "далеко"));
        QVERIFY(model()->moveSection(QStringLiteral("А–А"), "25"));
        QVERIFY(model()->addSectionToDrawing("left", 60, 105, 1.0, "40", true));
        QCOMPARE(doc().objectsOf<drawing::Hatch>().size(), std::size_t(4));
        doc().undo();
        QCOMPARE(doc().objectsOf<drawing::Hatch>().size(), std::size_t(2));
        view()->fitAll();
        QCOMPARE(window_->runCheck(), 0);
        QVERIFY(view()->grab().save("ui_model_section.png"));
        QVERIFY(ui::exportPdf(view()->currentScene(), "ui_model_section.pdf"));
        QVERIFY(window_->addDrawing("Ещё чертёж"));
        QCOMPARE(window_->drawingTabs()->count(), 4);
        QCOMPARE(QString::fromStdString(doc().name), QStringLiteral("Ещё чертёж"));

        showModel();
        model()->selectDocument(documentNamed("Опора")->id());
        QVERIFY(model()->exportStep("ui_support.step"));
        QVERIFY(model()->exportStl("ui_support.stl"));
        QVERIFY(QFileInfo("ui_support.step").size() > 5000);
        QVERIFY(QFileInfo("ui_support.stl").size() > 5000);
        QVERIFY(model()->grab().save("ui_model_part.png"));
        model()->selectDocument(documentNamed("Опора в сборе")->id());
        QCOMPARE(model()->view()->bodyCount(), std::size_t(2));
        QVERIFY(model()->exportStep("ui_assembly.step"));
        QVERIFY(model()->exportBillOfMaterials("ui_bom.csv"));
        QFile bom("ui_bom.csv");
        QVERIFY(bom.open(QIODevice::ReadOnly));
        const QString table = QString::fromUtf8(bom.readAll());
        QVERIFY(table.contains(QStringLiteral("\"КСМ.003.001\";\"Опора\";1;")));
        QVERIFY(table.contains(QStringLiteral("Палец")));
        model()->selectDocument(documentNamed("Опора")->id());
        QVERIFY(!model()->exportBillOfMaterials("ui_bom_bad.csv"));  // у детали состава нет
        model()->selectDocument(documentNamed("Опора в сборе")->id());
        QVERIFY(model()->grab().save("ui_model_assembly.png"));
    }

    // Отверстие и выступ ставятся щелчком по грани; размеры эскиза правятся в окне.
    void holeAndBossArePlacedByClickOnFace() {
        markSaved();
        window_->newProjectWithDemo();
        showModel();
        document::Document* support = documentNamed("Опора");
        model()->selectDocument(support->id());
        QVERIFY(!model()->pickedPointOnFace().has_value());  // ничего не выбрано
        QVERIFY(!model()->addShapeOnPickedFace(true, "10", "10", "4", "", "3", true));

        // Щелчок по верхней грани плиты в изометрии: точка щелчка переводится в координаты эскиза.
        QVERIFY(pickFace("E1/end"));
        const auto at = model()->pickedPointOnFace();
        QVERIFY(at.has_value());
        QVERIFY(at->x > 0.0 && at->x < 80.0 && at->y > 0.0 && at->y < 50.0);  // точка лежит на плите 80×50
        const double before = volumeOf(*support);
        const QString x = QString::number(at->x, 'f', 3), y = QString::number(at->y, 'f', 3);
        QVERIFY(model()->addShapeOnPickedFace(true, x, y, "4", "", "3", true));
        QCOMPARE(model()->tree()->count(), 6);
        // Щелчок мог прийтись у края плиты, поэтому объём проверяется нестрого: он уменьшился, но не больше цилиндра ⌀4×3.
        QVERIFY(volumeOf(*support) < before && before - volumeOf(*support) <= M_PI * 4.0 * 3.0 + 1e-6);
        // Отверстие стоит там, где щёлкнули.
        const auto wall = context_->kernel().faceInfo(context_->modeling().body(*support), "E4/side/1000");
        QVERIFY(wall.ok());
        QVERIFY(std::abs(wall.value().point.x - at->x) < 2e-3 && std::abs(wall.value().point.y - at->y) < 2e-3);
        QVERIFY(std::abs(wall.value().radius - 2.0) < 1e-9);
        // После щелчка в ту же точку окна луч попадает в отверстие — значит, оно действительно под курсором.

        // Размер эскиза правится в окне: радиус отверстия 2 → 3 мм.
        QCOMPARE(model()->sketchValueLabels(5).size(), 3);
        QVERIFY(model()->sketchValueLabels(5).at(2).contains(QStringLiteral("радиус")));
        QVERIFY(model()->setSketchValue(5, 0, 2, "3"));
        QVERIFY(std::abs(context_->kernel().faceInfo(context_->modeling().body(*support), "E4/side/1000").value().radius - 3.0) < 1e-9);
        // Точное место и точный объём: отверстие ⌀4 глубиной 3 в середине свободного поля плиты.
        QVERIFY(model()->setSketchValue(5, 0, 0, "20"));
        QVERIFY(model()->setSketchValue(5, 0, 1, "длина / 8"));  // размер эскиза может быть формулой
        QVERIFY(model()->setSketchValue(5, 0, 2, "2"));
        QVERIFY(std::abs((before - volumeOf(*support)) - M_PI * 4.0 * 3.0) < 1e-3);
        // Массив: три таких отверстия в ряд с шагом 12 мм, затем четыре по кругу, затем без массива.
        QVERIFY(model()->setPattern(5, 1, "3", "12", "0"));
        QVERIFY(std::abs((before - volumeOf(*support)) - 3 * M_PI * 4.0 * 3.0) < 1e-3);
        QVERIFY(model()->tree()->item(5)->text().contains(QStringLiteral("в ряд × 3")));
        QVERIFY(!model()->setPattern(5, 1, "2.5", "12", "0"));  // дробное число копий — объяснение в строке сообщений
        QVERIFY(model()->messages()->text().contains(QStringLiteral("целым")));
        QVERIFY(!model()->setPattern(3, 1, "3", "12", "0"));    // у скругления массива не бывает
        QVERIFY(model()->setPattern(5, 0, "", "", ""));
        QVERIFY(std::abs((before - volumeOf(*support)) - M_PI * 4.0 * 3.0) < 1e-3);
        QVERIFY(!model()->setSketchValue(5, 0, 2, "нет_такого"));
        QVERIFY(!model()->setSketchValue(5, 3, 0, "1"));
        QCOMPARE(model()->sketchValueLabels(0).size(), 16);  // плита: прямоугольник и четыре отверстия
        QVERIFY(model()->sketchValueLabels(3).isEmpty());    // у скругления эскиза нет

        // Прямоугольный выступ 6×4 высотой 5 на той же грани, вид сверху.
        model()->view()->fit(-90, 90);
        QCoreApplication::processEvents();
        QVERIFY(pickFace("E1/end"));
        const auto top = model()->pickedPointOnFace();
        QVERIFY(top.has_value());
        const double middle = volumeOf(*support);
        QVERIFY(model()->addShapeOnPickedFace(false, QString::number(top->x, 'f', 3), QString::number(top->y, 'f', 3),
                                              "6", "4", "5", false));
        // Выступ может частично лечь на скругление у бобышки, поэтому прибавка — не больше 6·4·5.
        QVERIFY(volumeOf(*support) > middle + 60.0 && volumeOf(*support) <= middle + 120.0 + 1e-6);
        // Цилиндрическая грань для эскиза не годится.
        model()->view()->fit(-60, 30);
        QCoreApplication::processEvents();
        QVERIFY(pickFace("E2/side/1000"));
        QVERIFY(!model()->pickedPointOnFace().has_value());
        QVERIFY(model()->grab().save("ui_model_on_face.png"));
    }

    void rotatingAndZoomingViewKeepsBodyVisible() {
        markSaved();
        window_->newProjectWithDemo();
        showModel();
        model()->selectDocument(documentNamed("Опора")->id());
        ui::ModelView3D* v = model()->view();
        const double yaw = v->camera().yawDegrees;
        const QPoint centre(v->width() / 2, v->height() / 2);
        QTest::mousePress(v, Qt::LeftButton, Qt::NoModifier, centre);
        QTest::mouseMove(v, centre + QPoint(60, 20));
        QTest::mouseRelease(v, Qt::LeftButton, Qt::NoModifier, centre + QPoint(60, 20));
        QVERIFY(std::abs(v->camera().yawDegrees - yaw) > 10.0);
        QVERIFY(v->pickedFaces().empty());  // протаскивание — не щелчок, грани не выбираются
        QVERIFY(v->faceAt(centre).body == 0);
        v->fit(-90, 90);
        QCOMPARE(QString::fromStdString(v->faceAt(QPoint(v->width() / 2, v->height() / 2)).faceName), QString());  // сквозное отверстие
    }

    // Схема собирается в окне: элементы, проводники щелчками по выводам, метки, проверка.
    void schematicIsBuiltWithToolsAndChecked() {
        QVERIFY(window_->placeSchematicElement("resistor", {81.7, 198.9}, 0, "1 кОм"));  // выравнивается по шагу 5 мм
        QVERIFY(window_->placeSchematicElement("capacitor", {120, 180}, 90, "10 нФ"));
        const auto elements = all<schematic::Element>();
        QCOMPARE(elements.size(), std::size_t(2));
        QVERIFY(std::abs(elements[0]->position.x - 80.0) < 1e-9 && std::abs(elements[0]->position.y - 200.0) < 1e-9);
        QCOMPARE(QString::fromStdString(elements[0]->designator), QStringLiteral("R1"));
        QCOMPARE(QString::fromStdString(elements[1]->designator), QStringLiteral("C1"));
        // Два элемента без проводников: четыре неподключённых вывода — видны в панели замечаний.
        QCOMPARE(window_->runSchematicCheck(), 4);

        // Проводник инструментом: щелчки рядом с выводами, привязка ставит концы точно на выводы.
        view()->setTool(ui::Tool::SchWire);
        click({90.4, 200.3});   // вывод 2 резистора (90; 200)
        click({120.2, 200.1});  // угол над конденсатором
        click({120.3, 190.2});  // вывод 2 конденсатора (120; 190)
        QTest::keyClick(view(), Qt::Key_Escape);
        const auto wires = all<schematic::Wire>();
        QCOMPARE(wires.size(), std::size_t(2));
        QVERIFY(geometry::distance(wires[0]->points.front(), {90, 200}) < 1e-9);
        QVERIFY(geometry::distance(wires[1]->points.back(), {120, 190}) < 1e-9);
        QVERIFY(window_->placeSchematicWire({50, 200}, {70, 200}));
        QVERIFY(window_->placeSchematicWire({120, 170}, {120, 150}));
        QVERIFY(window_->placeSchematicLabel({50, 200}, "Вход"));
        QVERIFY(window_->placeSchematicLabel({120, 150}, "Общий"));
        QCOMPARE(window_->runSchematicCheck(), 0);
        QVERIFY(window_->schematicNetsText().contains(QStringLiteral("«Вход» — R1:1")));
        QVERIFY(window_->schematicNetsText().contains(QStringLiteral("C1:2, R1:2")));

        // Номинал выбранного элемента и сдвиг с проводниками.
        view()->setTool(ui::Tool::Select);
        view()->select(elements[0]->id());
        QVERIFY(window_->setSchematicValue("2,2 кОм"));
        QCOMPARE(QString::fromStdString(all<schematic::Element>()[0]->value), QStringLiteral("2,2 кОм"));
        QVERIFY(window_->shiftSchematicElements(0, 10));
        QCOMPARE(window_->runSchematicCheck(), 0);  // проводники остались на выводах
        view()->clearSelection();
        QVERIFY(!window_->shiftSchematicElements(5, 0));
        QVERIFY(!window_->placeSchematicElement("triac", {50, 50}, 0, ""));
        // Перечень на лист: таблица из двух строк над основной надписью.
        QCOMPARE(window_->placeElementListOnSheet(), 2);

        QVERIFY(window_->exportElementList("ui_elements.csv"));
        QFile list("ui_elements.csv");
        QVERIFY(list.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(list.readAll()).contains(QStringLiteral("\"R1\";\"Резистор 2,2 кОм\";1;")));
    }

    // Все обозначения набора на одном листе — картинка для просмотра глазами.
    void everyLibrarySymbolCanBePlacedAndDrawn() {
        int index = 0;
        for (const auto& symbol : context_->schematic().library().symbols) {
            const double x = 40.0 + 28.0 * (index % 6), y = 265.0 - 28.0 * (index / 6);
            QVERIFY(window_->placeSchematicElement(QString::fromStdString(symbol.id), {x, y}, 0, ""));
            QVERIFY(window_->placeSchematicElement(QString::fromStdString(symbol.id), {x, y - 100.0}, 90, ""));
            ++index;
        }
        QCOMPARE(all<schematic::Element>().size(), std::size_t(2 * index));
        QVERIFY(ui::exportPdf(view()->currentScene(), "ui_symbols.pdf"));
    }

    void demoSchematicSheetOpensCleanAndRibbonHasPages() {
        markSaved();
        window_->newProjectWithDemo();
        window_->drawingTabs()->setCurrentIndex(2);
        QCOMPARE(QString::fromStdString(doc().name), QStringLiteral("Фильтр. Схема"));
        view()->fitAll();
        QCOMPARE(window_->runCheck(), 0);           // оформление листа
        QCOMPARE(window_->runSchematicCheck(), 0);  // сама схема
        QCOMPARE(window_->renumberSchematic(), 0);  // обозначения уже по порядку
        QVERIFY(window_->numberSchematicNets() >= 1);  // цепи без имени получили номера
        QCOMPARE(window_->numberSchematicNets(), 0);
        QCOMPARE(window_->runSchematicCheck(), 0);
        QCOMPARE(window_->ribbon()->pageCount(), 6);
        window_->ribbon()->setCurrentPage(2);
        QCoreApplication::processEvents();
        QVERIFY(window_->grab().save("ui_scheme.png"));
        QVERIFY(ui::exportPdf(view()->currentScene(), "ui_scheme.pdf"));
        // Кнопка ленты включает инструмент: «Проводник» на вкладке «Схема».
        bool found = false;
        for (auto* button : window_->ribbon()->findChildren<QToolButton*>())
            if (button->defaultAction() && button->defaultAction()->text() == QStringLiteral("Проводник")) {
                button->click();
                found = true;
            }
        QVERIFY(found);
        QCOMPARE(view()->tool(), ui::Tool::SchWire);
        // Кнопки масштаба поверх чертежа меняют вид и не меняют документ.
        const QPointF before = view()->toScreen({100, 100});
        view()->zoomBy(1.25);
        QVERIFY(view()->toScreen({100, 100}) != before);
        window_->ribbon()->setCurrentPage(1);
        QCoreApplication::processEvents();
        window_->drawingTabs()->setCurrentIndex(0);
        view()->fitAll();
        QVERIFY(window_->grab().save("ui_main.png"));
    }

    // Блок из выбранного, вставка, разбивка; протяжка контура с чертежа вдоль гнутой траектории.
    void blocksAndSweepWorkFromWindow() {
        auto& service = static_cast<drawing::IDrawingService&>(context_->drawing());
        QVERIFY(service.addCircle(doc(), {60, 220}, 4).ok());
        view()->selectAll();
        const std::size_t selected = view()->selection().size();
        QVERIFY(selected >= 1);
        // Протяжка: пруток ⌀8 вверх на 40, гиб радиусом 15, вбок на 50.
        showModel();
        QVERIFY(model()->newPart("Пруток"));
        QVERIFY(!model()->sweepContour("0 0", "0", false));      // у точки три координаты
        QVERIFY(model()->sweepContour("0 0 40; 50 0 40", "15", false));
        const double axis = (40.0 - 15) + (50.0 - 15) + M_PI / 2 * 15;
        QVERIFY(std::abs(volumeOf(*model()->currentDocument()) - M_PI * 16 * axis) < M_PI * 16 * axis * 1e-3);
        QVERIFY(model()->grab().save("ui_model_sweep.png"));

        // Блок: окружность заменяется вставкой; вторая вставка; разбивка возвращает обычную окружность.
        const std::size_t entities = doc().objectsOf<drawing::Entity>().size();
        QVERIFY(window_->makeBlockFromSelection(QStringLiteral("Отверстие")));
        QCOMPARE(doc().objectsOf<drawing::BlockReference>().size(), std::size_t(1));
        QCOMPARE(doc().objectsOf<drawing::Entity>().size(), entities - selected + 1);
        QVERIFY(window_->insertBlock(QStringLiteral("Отверстие"), 120, 220, 0));
        QVERIFY(!window_->insertBlock(QStringLiteral("Нет такого"), 0, 0, 0));
        QCOMPARE(doc().objectsOf<drawing::BlockReference>().size(), std::size_t(2));
        QVERIFY(!window_->explodeSelectedBlock());  // ничего не выбрано
        view()->select(doc().objectsOf<drawing::BlockReference>().back()->id());
        QVERIFY(window_->explodeSelectedBlock());
        QCOMPARE(doc().objectsOf<drawing::BlockReference>().size(), std::size_t(1));
    }

    // Ограничения эскиза: четырёхугольник «от руки» становится точным прямоугольником 60×35.
    void sketchConstraintsShapeRoughDrawing() {
        using sketcher::ConstraintKind;
        auto& service = static_cast<drawing::IDrawingService&>(context_->drawing());
        const std::vector<core::Uuid> s = {service.addLine(doc(), {40, 150}, {98, 153}).value(), service.addLine(doc(), {99, 152}, {97, 186}).value(),
                                           service.addLine(doc(), {98, 187}, {41, 184}).value(), service.addLine(doc(), {39, 185}, {41, 151}).value()};
        const auto line = [&](std::size_t i) { return doc().findAs<drawing::LineEntity>(s[i]); };
        QCOMPARE(window_->constrainSelection(ConstraintKind::Horizontal), 0);  // ничего не выбрано
        // Деталь с параметром «высота»: от него будет зависеть размер эскиза.
        view()->select(s[1]);
        QCOMPARE(window_->constrainSelection(ConstraintKind::Length, QStringLiteral("высота")), 0);  // детали ещё нет
        showModel();
        QVERIFY(model()->newPart("По эскизу"));
        QVERIFY(model()->addParameter("высота", "35"));
        for (std::size_t i = 0; i < 4; ++i) {
            view()->selectMany({s[i], s[(i + 1) % 4]});
            QCOMPARE(window_->constrainSelection(ConstraintKind::Coincident), 1);
        }
        view()->selectMany({s[0], s[2]});
        QCOMPARE(window_->constrainSelection(ConstraintKind::Horizontal), 2);
        view()->selectMany({s[1], s[3]});
        QCOMPARE(window_->constrainSelection(ConstraintKind::Vertical), 2);
        view()->select(s[0]);
        QCOMPARE(window_->constrainSelection(ConstraintKind::Length, 60.0), 1);
        view()->select(s[1]);
        QCOMPARE(window_->constrainSelection(ConstraintKind::Length, QStringLiteral("нет_такого_параметра")), 0);
        QCOMPARE(window_->constrainSelection(ConstraintKind::Length, QStringLiteral("высота")), 1);  // размер — формула
        QVERIFY(std::abs(geometry::distance(line(2)->start, line(2)->end) - 60.0) < 1e-6);
        QVERIFY(std::abs(geometry::distance(line(3)->start, line(3)->end) - 35.0) < 1e-6);
        QVERIFY(geometry::distance(line(0)->end, line(1)->start) < 1e-6);
        QVERIFY2(window_->constraintsText().contains(QStringLiteral("свободных движений — 2")), qPrintable(window_->constraintsText()));

        // Противоречащий размер отклоняется; «Изменить размер» перестраивает фигуру.
        view()->select(s[2]);
        QCOMPARE(window_->constrainSelection(ConstraintKind::Length, 80.0), 0);
        view()->select(s[0]);
        QVERIFY(window_->setConstraintValueOfSelection(90.0));
        QVERIFY(std::abs(geometry::distance(line(2)->start, line(2)->end) - 90.0) < 1e-6);
        // Закрепили сторону — эскиз определён полностью.
        QCOMPARE(window_->constrainSelection(ConstraintKind::Fixed), 1);
        QVERIFY(window_->constraintsText().contains(QStringLiteral("определён полностью")));
        // Окружность касается нижней стороны.
        const auto circle = service.addCircle(doc(), {70, 165}, 8).value();
        view()->selectMany({circle, s[0]});
        QCOMPARE(window_->constrainSelection(ConstraintKind::Tangent), 1);
        const auto* c = doc().findAs<drawing::CircleEntity>(circle);
        QVERIFY(std::abs(std::abs(c->center.y - line(0)->start.y) - c->radius) < 1e-6);
        view()->fitAll();
        QVERIFY(view()->grab().save("ui_sketch.png"));
        // Значки ограничений показаны поверх чертежа; у размера-формулы видна формула.
        const auto glyphs = view()->constraintGlyphs();
        QVERIFY(glyphs.size() >= 10);
        QVERIFY(std::any_of(glyphs.begin(), glyphs.end(), [](const auto& g) { return g.text.contains(QStringLiteral("(высота)")); }));
        // Контур выдавлен в деталь; изменили параметр детали — перестроились и контур на чертеже, и тело.
        view()->selectMany(s);
        QVERIFY(model()->extrudeContour("10", "0", false));
        document::Document& sketched = *model()->currentDocument();
        QVERIFY(std::abs(volumeOf(sketched) - 90.0 * 35 * 10) < 1e-3);
        QVERIFY(model()->setParameter("высота", "50"));
        QVERIFY(std::abs(geometry::distance(line(3)->start, line(3)->end) - 50.0) < 1e-6);
        QVERIFY(std::abs(volumeOf(sketched) - 90.0 * 50 * 10) < 1e-3);
        // Снятие ограничений с выбранного.
        view()->select(circle);
        QCOMPARE(window_->removeConstraintsOfSelection(), 1);
        QVERIFY(window_->solveConstraints());

        // Расстояние между центрами двух окружностей.
        const auto c1 = service.addCircle(doc(), {200, 200}, 5).value();
        const auto c2 = service.addCircle(doc(), {230, 205}, 5).value();
        view()->selectMany({c1, c2});
        QCOMPARE(window_->constrainSelection(ConstraintKind::Distance, 40.0), 1);
        QVERIFY(std::abs(geometry::distance(doc().findAs<drawing::CircleEntity>(c1)->center, doc().findAs<drawing::CircleEntity>(c2)->center) - 40.0) < 1e-6);
        // Точка на прямой: ближайший конец одного отрезка ложится на прямую другого.
        const auto base = service.addLine(doc(), {300, 100}, {360, 100}).value();
        const auto post = service.addLine(doc(), {330, 103}, {330, 150}).value();
        view()->selectMany({base, post});
        QCOMPARE(window_->constrainSelection(ConstraintKind::PointOnLine), 1);
        {
            const auto* l = doc().findAs<drawing::LineEntity>(base);
            const auto* q = doc().findAs<drawing::LineEntity>(post);
            const double dx = l->end.x - l->start.x, dy = l->end.y - l->start.y;
            QVERIFY(std::abs((q->start.x - l->start.x) * dy - (q->start.y - l->start.y) * dx) / std::hypot(dx, dy) < 1e-6);
        }
        view()->selectMany({base, c1});
        QCOMPARE(window_->constrainSelection(ConstraintKind::PointOnLine), 0);  // нужны два отрезка
        // Симметрия: ось программа находит сама — это средний отрезок.
        const auto left = service.addLine(doc(), {380, 110}, {385, 150}).value();
        const auto axis = service.addLine(doc(), {400, 100}, {400, 160}).value();
        const auto right = service.addLine(doc(), {421, 111}, {414, 149}).value();
        view()->selectMany({left, right});
        QCOMPARE(window_->constrainSelection(ConstraintKind::Symmetric), 0);  // нужны три отрезка
        view()->selectMany({left, axis, right});
        QCOMPARE(window_->constrainSelection(ConstraintKind::Symmetric), 2);
        {
            const auto* l = doc().findAs<drawing::LineEntity>(left);
            const auto* r = doc().findAs<drawing::LineEntity>(right);
            const auto* m = doc().findAs<drawing::LineEntity>(axis);
            // Отражение концов левого отрезка относительно оси совпадает с концами правого.
            const auto mirrored = [&](geometry::Point2 p) {
                const double dx = m->end.x - m->start.x, dy = m->end.y - m->start.y;
                const double t = ((p.x - m->start.x) * dx + (p.y - m->start.y) * dy) / (dx * dx + dy * dy);
                return geometry::Point2{2 * (m->start.x + t * dx) - p.x, 2 * (m->start.y + t * dy) - p.y};
            };
            QVERIFY(geometry::distance(mirrored(l->start), r->start) < 1e-4);
            QVERIFY(geometry::distance(mirrored(l->end), r->end) < 1e-4);
        }
    }

    void demoDrawingOpensWithLayersAndNoViolations() {
        markSaved();
        window_->newProjectWithDemo();
        // Дополнения: без них — подсказка, где их искать; после загрузки пример виден в списке.
        QVERIFY(window_->pluginsText().contains(QStringLiteral("не найдено")));
        QVERIFY(!window_->runPluginCommand(QStringLiteral("mass-report.csv")));
        QCOMPARE(context_->plugins().loadFrom(std::filesystem::path(COSMOCAD_TEST_PLUGINS)), 1);
        QVERIFY(window_->pluginsText().contains(QStringLiteral("Ведомость масс")));
        QVERIFY(window_->pluginsText().contains(QStringLiteral("работает")));
        QVERIFY(!window_->runPluginCommand(QStringLiteral("mass-report.unknown")));
        // Выключение дополнения записывается и действует со следующего запуска; включение убирает запись.
        QVERIFY(window_->setPluginEnabled(QStringLiteral("mass-report"), false));
        QCOMPARE(context_->disabledPlugins().size(), std::size_t(1));
        QVERIFY(window_->setPluginEnabled(QStringLiteral("mass-report"), false));
        QCOMPARE(context_->disabledPlugins().size(), std::size_t(1));
        QVERIFY(window_->setPluginEnabled(QStringLiteral("mass-report"), true));
        QVERIFY(context_->disabledPlugins().empty());
        QCOMPARE(window_->layerList()->count(), 4);
        QCOMPARE(window_->runCheck(), 0);
    }
};

QTEST_MAIN(UiTest)
#include "test_ui.moc"
