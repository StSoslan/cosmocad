#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTabBar>
#include <QToolButton>
#include <QEvent>
#include <QMenu>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <cmath>
#include <QVBoxLayout>
#include <fstream>

#include "Ribbon.h"
#include "ScenePainter.h"
#include "cosmocad/core/Version.h"
#include "cosmocad/exchange/Exchange.h"

namespace cosmocad::ui {

namespace fs = std::filesystem;

namespace {

fs::path toPath(const QString& text) { return fs::path(text.toStdU16String()); }
QString fromPath(const fs::path& path) { return QString::fromStdU16String(path.u16string()); }

}  // namespace

MainWindow::MainWindow(app::AppContext& context, QWidget* parent) : QMainWindow(parent), context_(context) {
    buildUi();
    resize(1280, 860);
    refreshActions();
}

void MainWindow::buildUi() {
    setWindowTitle(QString::fromUtf8(core::kAppName));

    view_ = new DrawingView(context_, this);
    setCentralWidget(view_);

    // --- Меню «Файл» ---
    QMenu* file = menuBar()->addMenu(tr("&Файл"));
    file->addAction(tr("&Новый чертёж"), QKeySequence::New, this, &MainWindow::onNew);
    file->addAction(tr("Открыть &пример"), this, [this] {
        if (confirmDiscard()) newProjectWithDemo();
    });
    file->addAction(tr("&Открыть…"), QKeySequence::Open, this, &MainWindow::onOpen);
    file->addAction(tr("&Сохранить"), QKeySequence::Save, this, &MainWindow::onSave);
    file->addAction(tr("Сохранить &как…"), QKeySequence::SaveAs, this, &MainWindow::onSaveAs);
    file->addSeparator();
    file->addAction(tr("&Экспорт (PDF, SVG, DXF)…"), this, &MainWindow::onExport);
    file->addAction(tr("&Печать…"), QKeySequence::Print, this, &MainWindow::onPrint);
    file->addSeparator();
    file->addAction(tr("В&ыход"), QKeySequence::Quit, this, &QWidget::close);

    // --- Меню «Правка» ---
    QMenu* edit = menuBar()->addMenu(tr("&Правка"));
    undoAction_ = edit->addAction(tr("&Отменить"), QKeySequence::Undo, this, &MainWindow::onUndo);
    redoAction_ = edit->addAction(tr("&Повторить"), QKeySequence::Redo, this, &MainWindow::onRedo);
    edit->addSeparator();
    edit->addAction(tr("Выбрать &всё"), QKeySequence::SelectAll, view_, &DrawingView::selectAll);
    edit->addAction(tr("&Удалить выбранное"), QKeySequence::Delete, view_, &DrawingView::deleteSelection);

    // --- Меню «Чертёж» ---
    QMenu* drawingMenu = menuBar()->addMenu(tr("&Чертёж"));
    drawingMenu->addAction(tr("&Лист и основная надпись…"), this, &MainWindow::onSheetSetup);
    drawingMenu->addAction(tr("&Проверить оформление"), QKeySequence(Qt::Key_F7), this, [this] { runCheck(); });
    drawingMenu->addAction(tr("&Новый чертёж в проекте…"), this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Новый чертёж"), tr("Название:"), QLineEdit::Normal,
                                                   tr("Чертёж"), &ok);
        if (ok && !name.trimmed().isEmpty()) addDrawing(name);
    });

    // --- Меню «Модель» ---
    QMenu* modelMenu = menuBar()->addMenu(tr("&Модель"));
    modelMenu->addAction(tr("Окно &3D-модели"), QKeySequence(Qt::Key_F3), this, [this] {
        modelWindow()->show();
        modelWindow()->raise();
        modelWindow()->activateWindow();
    });
    modelMenu->addAction(tr("&Обновить виды чертежа по модели"), this, [this] {
        if (document::Document* document = view_->document()) {
            if (auto updated = context_.updateModelViews(*document); !updated) report(updated.error());
        }
    });

    // --- Меню «Вид» ---
    QMenu* viewMenu = menuBar()->addMenu(tr("&Вид"));
    viewMenu->addAction(tr("Показать &всё"), QKeySequence(Qt::Key_Home), view_, &DrawingView::fitAll);

    // --- Инструменты ---
    // --- Лента команд вместо меню и панелей инструментов ---
    setStyleSheet(applicationStyle());
    menuBar()->hide();
    // Команды меню добавляются в окно, чтобы их сочетания клавиш работали и без строки меню.
    for (QMenu* menu : {file, edit, drawingMenu, modelMenu, viewMenu}) addActions(menu->actions());

    ribbon_ = new Ribbon(this);
    ribbon_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QToolBar* ribbonBar = addToolBar(tr("Лента"));
    ribbonBar->setMovable(false);
    ribbonBar->setFloatable(false);
    ribbonBar->addWidget(ribbon_);
    const int filePage = ribbon_->addPage(tr("Файл"));
    const int homePage = ribbon_->addPage(tr("Главная"));
    const int schemePage = ribbon_->addPage(tr("Схема"));
    const int modelPage = ribbon_->addPage(tr("3D"));
    const int viewPage = ribbon_->addPage(tr("Вид"));
    const int sketchPage = ribbon_->addPage(tr("Эскиз"));

    // Вкладка «Файл»: те же команды, что были в меню.
    {
        const QStringList shortNames = {tr("Новый"), tr("Пример"), tr("Открыть"), tr("Сохранить"), tr("Сохранить как"),
                                        tr("Экспорт"), tr("Печать")};
        RibbonGroup* project = ribbon_->addGroup(filePage, tr("Проект"));
        RibbonGroup* output = ribbon_->addGroup(filePage, tr("Вывод"));
        int index = 0;
        for (QAction* action : file->actions()) {
            if (action->isSeparator() || index >= shortNames.size()) continue;
            action->setIconText(shortNames.at(index));
            (index < 5 ? project : output)->addAction(action);
            ++index;
        }
        RibbonGroup* history = ribbon_->addGroup(filePage, tr("История"));
        // Дополнения: кнопка на каждую команду загруженных дополнений и список самих дополнений.
        RibbonGroup* addons = ribbon_->addGroup(filePage, tr("Дополнения"));
        addons->setCompact(true);
        for (const auto* command : context_.plugins().commands()) {
            QAction* action = addons->addAction(QString::fromStdString(command->title));
            action->setToolTip(QString::fromStdString(command->description));
            const QString id = QString::fromStdString(command->id);
            connect(action, &QAction::triggered, this, [this, id] { runPluginCommand(id); });
        }
        QAction* addonList = addons->addAction(tr("Список дополнений"));
        addonList->setToolTip(tr("Какие дополнения найдены и работают"));
        connect(addonList, &QAction::triggered, this, [this] { QMessageBox::information(this, tr("Дополнения"), pluginsText()); });
        QAction* addonSwitch = addons->addAction(tr("Включить или выключить…"));
        addonSwitch->setToolTip(tr("Выключенное дополнение не загружается при следующем запуске"));
        connect(addonSwitch, &QAction::triggered, this, [this] {
            const auto disabled = context_.disabledPlugins();
            QStringList names;
            std::vector<std::pair<QString, bool>> items;  // имя и «сейчас выключено»
            for (const auto& plugin : context_.plugins().plugins()) {
                const bool off = std::find(disabled.begin(), disabled.end(), plugin.id) != disabled.end();
                names << QString::fromStdString(plugin.name) + (off ? tr(" — включить") : tr(" — выключить"));
                items.emplace_back(QString::fromStdString(plugin.id), off);
            }
            if (names.isEmpty()) {
                statusBar()->showMessage(tr("Дополнений не найдено."), 6000);
                return;
            }
            bool ok = false;
            const QString choice = QInputDialog::getItem(this, tr("Дополнения"), tr("Что сделать:"), names, 0, false, &ok);
            if (!ok) return;
            const auto& item = items[static_cast<std::size_t>(names.indexOf(choice))];
            setPluginEnabled(item.first, item.second);
        });
        undoAction_->setIconText(tr("Отменить"));
        redoAction_->setIconText(tr("Повторить"));
        history->addAction(undoAction_);
        history->addAction(redoAction_);
    }

    RibbonGroup* tools = ribbon_->addGroup(homePage, tr("Черчение"));
    auto* group = new QActionGroup(this);
    auto addTool = [&](const QString& title, Tool tool, const QString& hint) {
        QAction* action = tools->addAction(title);
        action->setCheckable(true);
        action->setToolTip(hint);
        action->setStatusTip(hint);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool, hint] {
            view_->setTool(tool);
            statusBar()->showMessage(hint);
            view_->setFocus();
        });
        return action;
    };
    addTool(tr("Выбор"), Tool::Select, tr("Щелчок — выбрать элемент, протяжка — выбрать рамкой, Shift — добавить"))
        ->setChecked(true);
    addTool(tr("Отрезок"), Tool::Line, tr("Отрезок: начало, затем конец. Esc — закончить"));
    addTool(tr("Прямоугольник"), Tool::Rectangle, tr("Прямоугольник: два противоположных угла"));
    addTool(tr("Окружность"), Tool::Circle, tr("Окружность: центр, затем точка на окружности или радиус числом"));
    addTool(tr("Дуга"), Tool::Arc, tr("Дуга: центр, начало, конец — против часовой стрелки"));

    tools = ribbon_->addGroup(homePage, tr("Правка"));
    tools->setCompact(true);
    addTool(tr("Перенос"), Tool::Move, tr("Перенос выбранного: откуда, затем куда"));
    addTool(tr("Копия"), Tool::Copy, tr("Копия выбранного: откуда, затем куда. Esc — закончить"));
    addTool(tr("Поворот"), Tool::Rotate, tr("Поворот выбранного: центр, затем направление или угол в градусах числом"));
    addTool(tr("Отражение"), Tool::Mirror, tr("Отражение выбранного: две точки оси"));
    addTool(tr("Обрезка"), Tool::Trim, tr("Обрезка: щёлкните по лишней части отрезка — она удалится до ближайших пересечений"));
    addTool(tr("Скругление"), Tool::Fillet, tr("Скругление: щёлкните по двум отрезкам. Радиус — числом в поле команды до щелчков"));
    addTool(tr("Фаска"), Tool::Chamfer, tr("Фаска: щёлкните по двум отрезкам. Размер — числом в поле команды до щелчков"));

    tools = ribbon_->addGroup(homePage, tr("Оформление"));
    addTool(tr("Текст"), Tool::Text, tr("Текст: укажите место, затем введите текст"));
    addTool(tr("Размер"), Tool::Dimension, tr("Линейный размер: две точки и положение размерной линии"));
    addTool(tr("Выноска"), Tool::Leader, tr("Выноска: куда указывает стрелка, затем место излома; после этого введите текст"));
    addTool(tr("Шероховатость"), Tool::Roughness,
            tr("Знак шероховатости: укажите точку на линии поверхности, выносной линии или полке выноски, затем введите значение"));
    {
        QAction* blockAction = tools->addAction(tr("Блок из выбранного…"));
        blockAction->setToolTip(tr("Собрать выбранные элементы в блок, который можно вставлять много раз"));
        connect(blockAction, &QAction::triggered, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Блок"), tr("Имя блока:"), QLineEdit::Normal, tr("Блок 1"), &ok);
            if (ok && !name.trimmed().isEmpty()) makeBlockFromSelection(name.trimmed());
        });
        QAction* insertAction = tools->addAction(tr("Вставить блок…"));
        insertAction->setToolTip(tr("Поставить на чертёж ещё одну вставку готового блока"));
        connect(insertAction, &QAction::triggered, this, [this] {
            document::Document* document = view_->document();
            if (!document) return;
            QStringList names;
            for (const auto& name : context_.drawing().blockNames(*document)) names << QString::fromStdString(name);
            if (names.isEmpty()) {
                statusBar()->showMessage(tr("На чертеже нет блоков: выберите элементы и нажмите «Блок из выбранного…»."), 8000);
                return;
            }
            bool ok = false;
            const QString name = QInputDialog::getItem(this, tr("Вставить блок"), tr("Какой блок:"), names, 0, false, &ok);
            if (!ok) return;
            const double x = QInputDialog::getDouble(this, tr("Вставить блок"), tr("X базовой точки, мм:"), 100, -100000, 100000, 2, &ok);
            if (!ok) return;
            const double y = QInputDialog::getDouble(this, tr("Вставить блок"), tr("Y базовой точки, мм:"), 100, -100000, 100000, 2, &ok);
            if (!ok) return;
            const double angle = QInputDialog::getDouble(this, tr("Вставить блок"), tr("Поворот, градусы:"), 0, -360, 360, 1, &ok);
            if (ok) insertBlock(name, x, y, angle);
        });
        QAction* explodeAction = tools->addAction(tr("Разбить блок"));
        explodeAction->setToolTip(tr("Превратить выбранную вставку блока в обычные линии"));
        connect(explodeAction, &QAction::triggered, this, [this] { explodeSelectedBlock(); });
        QAction* requirementsAction = tools->addAction(tr("Технические требования…"));
        requirementsAction->setToolTip(tr("Пронумерованные требования над основной надписью"));
        connect(requirementsAction, &QAction::triggered, this, [this] {
            document::Document* document = view_->document();
            if (!document) return;
            QStringList now;
            for (const auto& item : context_.technicalRequirements(*document)) now << QString::fromStdString(item);
            bool ok = false;
            const QString edited = QInputDialog::getMultiLineText(
                this, tr("Технические требования"),
                tr("Каждый пункт — с новой строки, без номеров: программа пронумерует сама.\nПустое поле убирает требования."),
                now.join("\n"), &ok);
            if (!ok) return;
            QStringList items;
            for (const QString& line : edited.split('\n'))
                if (!line.trimmed().isEmpty()) items << line.trimmed();
            setTechnicalRequirements(items);
        });
        QAction* cornerAction = tools->addAction(tr("Шероховатость листа…"));
        cornerAction->setToolTip(tr("Общий знак шероховатости в правом верхнем углу чертежа"));
        connect(cornerAction, &QAction::triggered, this, [this] {
            bool ok = false;
            const QString value = QInputDialog::getText(this, tr("Шероховатость листа"),
                                                        tr("Значение, например «Ra 6,3». Пусто — убрать знак:"), QLineEdit::Normal,
                                                        QStringLiteral("Ra 6,3"), &ok);
            if (!ok) return;
            const bool others = !value.trimmed().isEmpty() &&
                                QMessageBox::question(this, tr("Шероховатость листа"),
                                                      tr("Знак относится к остальным поверхностям (на изображении есть другие знаки)?\n"
                                                         "«Да» — рядом будет знак в скобках.")) == QMessageBox::Yes;
            setSheetRoughness(value, 0, others);
        });
    }
    tools->setCompact(true);
    addTool(tr("Радиус"), Tool::Radius, tr("Размер радиуса: щелчок по окружности или дуге, затем направление стрелки"));
    addTool(tr("Диаметр"), Tool::Diameter, tr("Размер диаметра: щелчок по окружности или дуге, затем направление"));
    addTool(tr("Угол"), Tool::Angle, tr("Угловой размер: вершина, точка на первой стороне, на второй (против часовой стрелки), положение дуги"));
    QAction* hatchAction = tools->addAction(tr("Штриховка"));
    hatchAction->setToolTip(tr("Заштриховать область, ограниченную выбранными линиями"));
    connect(hatchAction, &QAction::triggered, this, &MainWindow::onHatch);

    tools = ribbon_->addGroup(homePage, tr("Слои"));
    QAction* newLayer = tools->addAction(tr("Новый слой"));
    newLayer->setToolTip(tr("Создать слой с выбранным типом линии"));
    connect(newLayer, &QAction::triggered, this, &MainWindow::onAddLayer);
    QAction* toLayer = tools->addAction(tr("На слой"));
    toLayer->setToolTip(tr("Перенести выбранные элементы на слой, выделенный в панели «Слои»"));
    connect(toLayer, &QAction::triggered, this, &MainWindow::onMoveToLayer);

    tools = ribbon_->addGroup(homePage, tr("Чертёж"));
    QAction* checkAction = tools->addAction(tr("Проверка"));
    checkAction->setToolTip(tr("Проверить оформление чертежа по профилю стандарта (F7)"));
    connect(checkAction, &QAction::triggered, this, [this] { runCheck(); });
    QAction* sheetAction = tools->addAction(tr("Лист"));
    sheetAction->setToolTip(tr("Формат листа и основная надпись"));
    connect(sheetAction, &QAction::triggered, this, &MainWindow::onSheetSetup);

    // Вкладка «Схема».
    // ---- Эскиз: ограничения ----
    {
        using sketcher::ConstraintKind;
        RibbonGroup* geometric = ribbon_->addGroup(sketchPage, tr("Ограничения формы"));
        geometric->setCompact(true);
        const auto simple = [&](RibbonGroup* group, const QString& title, const QString& tip, ConstraintKind kind) {
            QAction* action = group->addAction(title);
            action->setToolTip(tip);
            connect(action, &QAction::triggered, this, [this, kind] { constrainSelection(kind); });
        };
        simple(geometric, tr("Закрепить"), tr("Выбранные элементы остаются на месте, остальные подгоняются под них"), ConstraintKind::Fixed);
        simple(geometric, tr("Горизонтально"), tr("Выбранные отрезки становятся горизонтальными"), ConstraintKind::Horizontal);
        simple(geometric, tr("Вертикально"), tr("Выбранные отрезки становятся вертикальными"), ConstraintKind::Vertical);
        simple(geometric, tr("Соединить концы"), tr("Два отрезка: ближайшие друг к другу концы совмещаются"), ConstraintKind::Coincident);
        simple(geometric, tr("Параллельно"), tr("Два отрезка становятся параллельными"), ConstraintKind::Parallel);
        simple(geometric, tr("Перпендикулярно"), tr("Два отрезка становятся перпендикулярными"), ConstraintKind::Perpendicular);
        simple(geometric, tr("Равны"), tr("Два отрезка равной длины или две окружности равного радиуса"), ConstraintKind::EqualLength);
        simple(geometric, tr("Касание"), tr("Отрезок и окружность или две окружности касаются"), ConstraintKind::Tangent);
        simple(geometric, tr("Общий центр"), tr("Две окружности или дуги с общим центром"), ConstraintKind::Concentric);
        simple(geometric, tr("Точка на прямой"), tr("Два отрезка: конец одного ложится на прямую другого (берётся ближайший конец)"), ConstraintKind::PointOnLine);
        simple(geometric, tr("Симметрия"), tr("Три отрезка: два становятся зеркальными относительно третьего. Ось программа определяет сама"), ConstraintKind::Symmetric);

        RibbonGroup* sizes = ribbon_->addGroup(sketchPage, tr("Размеры"));
        sizes->setCompact(true);
        const auto sized = [&](const QString& title, const QString& tip, const QString& prompt, ConstraintKind kind, double initial) {
            QAction* action = sizes->addAction(title);
            action->setToolTip(tip);
            connect(action, &QAction::triggered, this, [this, kind, title, prompt, initial] {
                bool ok = false;
                const QString value = QInputDialog::getText(
                    this, title, prompt + tr("\nМожно ввести формулу от параметров детали, выбранной в окне 3D: «длина / 2»."),
                    QLineEdit::Normal, QString::number(initial), &ok);
                if (ok && !value.trimmed().isEmpty()) constrainSelection(kind, value);
            });
        };
        sized(tr("Длина…"), tr("Задать длину выбранных отрезков"), tr("Длина, мм:"), ConstraintKind::Length, 50);
        sized(tr("Радиус…"), tr("Задать радиус выбранных окружностей и дуг"), tr("Радиус, мм:"), ConstraintKind::Radius, 10);
        sized(tr("Угол…"), tr("Задать угол между двумя отрезками"), tr("Угол, градусы:"), ConstraintKind::Angle, 90);
        sized(tr("Расстояние…"), tr("Расстояние между ближайшими концами двух отрезков или центрами двух окружностей"), tr("Расстояние, мм:"), ConstraintKind::Distance, 20);
        QAction* change = sizes->addAction(tr("Изменить размер…"));
        change->setToolTip(tr("Новое значение длины или радиуса выбранного элемента: фигура перестроится"));
        connect(change, &QAction::triggered, this, [this] {
            bool ok = false;
            const double value = QInputDialog::getDouble(this, tr("Изменить размер"), tr("Новое значение, мм:"), 50, 0, 100000, 3, &ok);
            if (ok) setConstraintValueOfSelection(value);
        });

        RibbonGroup* service = ribbon_->addGroup(sketchPage, tr("Проверка"));
        QAction* list = service->addAction(tr("Состояние"));
        list->setToolTip(tr("Определён ли эскиз полностью, сколько свободных движений осталось, список ограничений"));
        connect(list, &QAction::triggered, this, [this] { QMessageBox::information(this, tr("Ограничения эскиза"), constraintsText()); });
        QAction* resolve = service->addAction(tr("Пересчитать"));
        resolve->setToolTip(tr("Подогнать фигуру под ограничения после ручной правки"));
        connect(resolve, &QAction::triggered, this, [this] { solveConstraints(); });
        QAction* drop = service->addAction(tr("Снять"));
        drop->setToolTip(tr("Убрать все ограничения с выбранных элементов"));
        connect(drop, &QAction::triggered, this, [this] { removeConstraintsOfSelection(); });
    }

    tools = ribbon_->addGroup(schemePage, tr("Элементы схемы"));
    {
        auto* chooser = new QWidget(this);
        auto* form = new QVBoxLayout(chooser);
        form->setContentsMargins(4, 0, 4, 0);
        symbolBox_ = new QComboBox(chooser);
        symbolBox_->setObjectName("symbol_box");
        for (const auto& symbol : context_.schematic().library().symbols)
            symbolBox_->addItem(QString::fromStdString(symbol.name), QString::fromStdString(symbol.id));
        symbolBox_->setToolTip(tr("Какое обозначение ставить инструментом «Элемент»"));
        rotationBox_ = new QComboBox(chooser);
        rotationBox_->setObjectName("rotation_box");
        rotationBox_->addItem(tr("Горизонтально"), 0);
        rotationBox_->addItem(tr("Вертикально"), 90);
        form->addWidget(symbolBox_);
        form->addWidget(rotationBox_);
        tools->addWidget(chooser);
    }
    addTool(tr("Элемент"), Tool::SchElement, tr("Элемент схемы: укажите место; программа спросит номинал. Место выравнивается по шагу 5 мм"));
    addTool(tr("Проводник"), Tool::SchWire, tr("Проводник: щёлкайте точки по очереди, привязка ловит выводы. Esc — закончить"));
    addTool(tr("Метка"), Tool::SchLabel, tr("Метка цепи: укажите конец проводника и введите имя цепи"));

    tools = ribbon_->addGroup(schemePage, tr("Правка схемы"));
    tools->setCompact(true);
    QAction* renumberAction = tools->addAction(tr("Перенумеровать"));
    renumberAction->setToolTip(tr("Выдать позиционные обозначения заново: слева направо, сверху вниз"));
    connect(renumberAction, &QAction::triggered, this, [this] { renumberSchematic(); });
    QAction* netNumbersAction = tools->addAction(tr("Номера цепей"));
    netNumbersAction->setToolTip(tr("Поставить номер на каждую цепь, у которой ещё нет имени"));
    connect(netNumbersAction, &QAction::triggered, this, [this] { numberSchematicNets(); });
    QAction* valueAction = tools->addAction(tr("Номинал…"));
    valueAction->setToolTip(tr("Изменить номинал выбранного элемента схемы"));
    connect(valueAction, &QAction::triggered, this, &MainWindow::onSchematicValue);
    QAction* shiftAction = tools->addAction(tr("Сдвинуть…"));
    shiftAction->setToolTip(tr("Сдвинуть выбранные элементы схемы; присоединённые проводники едут следом"));
    connect(shiftAction, &QAction::triggered, this, &MainWindow::onSchematicShift);

    tools = ribbon_->addGroup(schemePage, tr("Проверка и вывод"));
    tools->setCompact(false);
    QAction* schemeCheck = tools->addAction(tr("Проверить схему"));
    schemeCheck->setIconText(tr("Проверка"));
    schemeCheck->setToolTip(tr("Найти висящие проводники, неподключённые выводы, повторы обозначений"));
    connect(schemeCheck, &QAction::triggered, this, [this] { runSchematicCheck(); });
    QAction* netsAction = tools->addAction(tr("Цепи"));
    netsAction->setToolTip(tr("Показать цепи схемы: номер, имя, какие выводы соединены"));
    connect(netsAction, &QAction::triggered, this,
            [this] { QMessageBox::information(this, tr("Цепи схемы"), schematicNetsText()); });
    QAction* listAction = tools->addAction(tr("Перечень в CSV…"));
    listAction->setIconText(tr("Перечень"));
    listAction->setToolTip(tr("Сохранить перечень элементов таблицей (открывается в Excel)"));
    connect(listAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Перечень элементов"), QString(), tr("Таблица CSV (*.csv)"));
        if (!path.isEmpty()) exportElementList(toPath(path));
    });

    QAction* tableAction = tools->addAction(tr("Перечень на лист"));
    tableAction->setIconText(tr("На лист"));
    tableAction->setToolTip(tr("Поставить таблицу перечня элементов над основной надписью (ГОСТ 2.701)"));
    connect(tableAction, &QAction::triggered, this, [this] { placeElementListOnSheet(); });

    // Вкладка «3D».
    tools = ribbon_->addGroup(modelPage, tr("Модель"));
    QAction* open3d = tools->addAction(tr("Окно 3D"));
    open3d->setToolTip(tr("Открыть окно 3D-модели (F3)"));
    connect(open3d, &QAction::triggered, this, [this] {
        modelWindow()->show();
        modelWindow()->raise();
    });
    QAction* updateViews = tools->addAction(tr("Обновить виды"));
    updateViews->setToolTip(tr("Перестроить виды этого чертежа по текущей 3D-модели"));
    connect(updateViews, &QAction::triggered, this, [this] {
        if (document::Document* document = view_->document())
            if (auto updated = context_.updateModelViews(*document); !updated) report(updated.error());
    });

    // Вкладка «Вид».
    tools = ribbon_->addGroup(viewPage, tr("Масштаб"));
    connect(tools->addAction(tr("Показать всё")), &QAction::triggered, view_, &DrawingView::fitAll);
    connect(tools->addAction(tr("Ближе")), &QAction::triggered, this, [this] { view_->zoomBy(1.25); });
    connect(tools->addAction(tr("Дальше")), &QAction::triggered, this, [this] { view_->zoomBy(0.8); });
    ribbon_->setCurrentPage(homePage);

    // Кнопки вида поверх чертежа, справа.
    viewButtons_ = new QWidget(view_);
    viewButtons_->setObjectName("viewButtons");
    viewButtons_->setAttribute(Qt::WA_StyledBackground, true);
    {
        auto* column = new QVBoxLayout(viewButtons_);
        column->setContentsMargins(4, 4, 4, 4);
        column->setSpacing(2);
        const auto button = [&](const QString& title, auto handler) {
            auto* b = new QToolButton(viewButtons_);
            b->setIcon(commandIcon(title));
            b->setIconSize(QSize(22, 22));
            b->setToolTip(title);
            b->setCursor(Qt::ArrowCursor);
            connect(b, &QToolButton::clicked, this, handler);
            column->addWidget(b);
        };
        button(tr("Показать всё"), [this] { view_->fitAll(); });
        button(tr("Ближе"), [this] { view_->zoomBy(1.25); });
        button(tr("Дальше"), [this] { view_->zoomBy(0.8); });
        viewButtons_->adjustSize();
    }
    view_->installEventFilter(this);

    // --- Нижняя строка: вкладки чертежей, поле команды, положение курсора ---
    auto* bottomBar = new QToolBar(tr("Чертежи и команда"), this);
    bottomBar->setObjectName("bottomBar");
    bottomBar->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, bottomBar);
    drawings_ = new QTabBar(this);
    drawings_->setObjectName("drawing_tabs");
    drawings_->setDrawBase(false);
    drawings_->setExpanding(false);
    drawings_->setToolTip(tr("Чертежи и схемы проекта"));
    bottomBar->addWidget(drawings_);
    QAction* addDrawingAction = bottomBar->addAction(QStringLiteral("+"));
    addDrawingAction->setToolTip(tr("Новый чертёж в проекте"));
    connect(addDrawingAction, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Новый чертёж"), tr("Название:"), QLineEdit::Normal, tr("Чертёж"), &ok);
        if (ok && !name.trimmed().isEmpty()) addDrawing(name);
    });
    connect(drawings_, &QTabBar::currentChanged, this, [this](int index) {
        if (updatingDrawings_) return;
        auto id = core::Uuid::parse(drawings_->tabData(index).toString().toStdString());
        view_->setDocument(id ? context_.documents().findDocument(id.value()) : nullptr);
        violations_->clear();
        refreshActions();
        refreshLayers();
    });
    {
        auto* spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        bottomBar->addWidget(spacer);
    }
    input_ = new QLineEdit(this);
    input_->setObjectName("commandLine");
    input_->setPlaceholderText(tr("Команда:  X Y  ·  @dX dY  ·  число"));
    input_->setToolTip(tr("Точка с клавиатуры вместо щелчка: «100 50» — координаты, «@20 0» — смещение от "
                          "предыдущей точки, одно число — радиус или угол в градусах"));
    input_->setMinimumWidth(360);
    bottomBar->addWidget(input_);
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        if (view_->inputText(input_->text())) input_->clear();
    });
    {
        auto* spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        bottomBar->addWidget(spacer);
    }
    cursorLabel_ = new QLabel(this);
    cursorLabel_->setMinimumWidth(190);
    bottomBar->addWidget(cursorLabel_);
    connect(view_, &DrawingView::cursorMoved, cursorLabel_, &QLabel::setText);

    // --- Панель слоёв ---
    auto* layerDock = new QDockWidget(tr("Слои"), this);
    auto* layerPanel = new QWidget(layerDock);
    auto* layerLayout = new QVBoxLayout(layerPanel);
    layerLayout->setContentsMargins(4, 4, 4, 4);
    layers_ = new QListWidget(layerPanel);
    layers_->setToolTip(tr("Выделенный слой получает новые элементы. Галочка — слой виден."));
    auto* addLayerButton = new QPushButton(tr("Новый слой…"), layerPanel);
    auto* moveButton = new QPushButton(tr("Выбранное — на этот слой"), layerPanel);
    layerLayout->addWidget(layers_);
    layerLayout->addWidget(addLayerButton);
    layerLayout->addWidget(moveButton);
    layerDock->setWidget(layerPanel);
    addDockWidget(Qt::RightDockWidgetArea, layerDock);
    connect(addLayerButton, &QPushButton::clicked, this, &MainWindow::onAddLayer);
    connect(moveButton, &QPushButton::clicked, this, &MainWindow::onMoveToLayer);
    connect(layers_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (updatingLayers_ || !item) return;
        auto id = core::Uuid::parse(item->data(Qt::UserRole).toString().toStdString());
        view_->setCurrentLayer(id ? id.value() : core::Uuid());
    });
    connect(layers_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (updatingLayers_ || !view_->document()) return;
        auto id = core::Uuid::parse(item->data(Qt::UserRole).toString().toStdString());
        if (!id || id.value().isNil()) return;
        const auto* layer = view_->document()->findAs<drawing::Layer>(id.value());
        if (!layer) return;
        bool visible = item->checkState() == Qt::Checked;
        if (visible == layer->visible) return;
        if (auto changed = context_.drawing().setLayerState(*view_->document(), id.value(), visible, layer->locked);
            !changed)
            report(changed.error());
    });

    // --- Панель свойств ---
    auto* propertiesDock = new QDockWidget(tr("Свойства"), this);
    properties_ = new PropertiesPanel(context_, view_, propertiesDock);
    propertiesDock->setWidget(properties_);
    addDockWidget(Qt::LeftDockWidgetArea, propertiesDock);
    connect(properties_, &PropertiesPanel::message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });

    // --- Панель замечаний нормоконтроля ---
    auto* dock = new QDockWidget(tr("Замечания по оформлению"), this);
    violations_ = new QListWidget(dock);
    dock->setWidget(violations_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    // Размеры панелей задаются после показа окна: до этого Qt их не учитывает.
    QTimer::singleShot(0, this, [this, dock, layerDock] {
        resizeDocks({dock}, {130}, Qt::Vertical);
        resizeDocks({layerDock}, {240}, Qt::Horizontal);
        QTimer::singleShot(0, view_, &DrawingView::fitAll);  // показать лист целиком в новом размере окна
    });
    connect(violations_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        auto id = core::Uuid::parse(item->data(Qt::UserRole).toString().toStdString());
        if (id) view_->select(id.value());
    });

    connect(view_, &DrawingView::message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    connect(view_, &DrawingView::documentEdited, this, &MainWindow::refreshActions);
    // После любой правки чертежа фигура с ограничениями подгоняется заново.
    connect(view_, &DrawingView::documentEdited, this, [this] {
        document::Document* document = view_->document();
        if (!document || document->objectsOf<sketcher::Constraint>().empty()) return;
        (void)context_.sketcher().removeOrphans(*document);
        auto solved = context_.sketcher().solve(*document);
        if (solved && !solved.value().solved)
            statusBar()->showMessage(tr("После правки ограничения эскиза противоречат друг другу: фигура не подогнана."), 8000);
        syncPartsWithDrawing();
        view_->update();
    });
    connect(view_, &DrawingView::textRequested, this, &MainWindow::onTextRequested);
    connect(view_, &DrawingView::leaderRequested, this, &MainWindow::onLeaderRequested);
    connect(view_, &DrawingView::schematicElementRequested, this, [this](double x, double y) {
        bool ok = false;
        const QString value = QInputDialog::getText(this, tr("Элемент схемы"), tr("Номинал или тип (можно оставить пустым):"),
                                                    QLineEdit::Normal, QString(), &ok);
        if (ok) placeSchematicElement(symbolBox_->currentData().toString(), {x, y}, rotationBox_->currentData().toInt(), value);
    });
    connect(view_, &DrawingView::schematicWireRequested, this, [this](double x1, double y1, double x2, double y2) {
        placeSchematicWire({x1, y1}, {x2, y2});
    });
    connect(view_, &DrawingView::roughnessRequested, this, [this](double x, double y) {
        bool ok = false;
        const QStringList kinds = {tr("способ обработки не установлен"), tr("удалением слоя материала"), tr("без удаления материала")};
        const QString kind = QInputDialog::getItem(this, tr("Шероховатость"), tr("Вид знака:"), kinds, 0, false, &ok);
        if (!ok) return;
        const QString value = QInputDialog::getText(this, tr("Шероховатость"),
                                                    tr("Значение с символом параметра, например «Ra 3,2» или «Rz 50».\nПусто — знак без значения:"),
                                                    QLineEdit::Normal, QStringLiteral("Ra 3,2"), &ok);
        if (!ok) return;
        int turns = 0;
        if (value.trimmed().isEmpty()) {
            const QStringList sides = {tr("на линии сверху"), tr("на линии слева"), tr("на линии снизу"), tr("на линии справа")};
            const QString side = QInputDialog::getItem(this, tr("Шероховатость"), tr("С какой стороны линии стоит знак:"), sides, 0, false, &ok);
            if (!ok) return;
            turns = sides.indexOf(side);
        }
        view_->placeRoughness({x, y}, kinds.indexOf(kind), value, turns);
    });
    connect(view_, &DrawingView::schematicLabelRequested, this, [this](double x, double y) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Метка цепи"), tr("Имя цепи:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !name.trimmed().isEmpty()) placeSchematicLabel({x, y}, name);
    });
    connect(view_, &DrawingView::documentChanged, this, [this] {
        refreshActions();
        refreshLayers();
    });

    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setInterval(2 * 60 * 1000);  // интервал автосохранения — 2 минуты
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::onAutosave);
    autosaveTimer_->start();
}

void MainWindow::report(const core::Error& error) {
    statusBar()->showMessage(QString::fromStdString(error.message), 6000);
}

void MainWindow::showFirstDrawing() {
    document::Document* first = nullptr;
    for (const auto& document : context_.documents().project().documents())
        if (document->kind() == document::DocumentKind::Drawing) {
            first = document.get();
            break;
        }
    updatingDrawings_ = true;
    while (drawings_->count() > 0) drawings_->removeTab(0);
    for (const auto& document : context_.documents().project().documents())
        if (document->kind() == document::DocumentKind::Drawing) {
            const int tab = drawings_->addTab(QString::fromStdString(document->name));
            drawings_->setTabData(tab, QString::fromStdString(document->id().toString()));
        }
    if (first) drawings_->setCurrentIndex(0);
    updatingDrawings_ = false;
    view_->setDocument(first);
    violations_->clear();
    refreshActions();
    refreshLayers();
    if (modelWindow_) modelWindow_->refresh();
}

ModelWindow* MainWindow::modelWindow() {
    if (!modelWindow_) {
        modelWindow_ = new ModelWindow(
            context_, [this] { return view_->document(); },
            [this] { return std::vector<core::Uuid>(view_->selection().begin(), view_->selection().end()); }, this);
        modelWindow_->setWindowFlag(Qt::Window);
        connect(modelWindow_, &ModelWindow::drawingChanged, this, [this] {
            refreshActions();
            view_->update();
        });
        connect(modelWindow_, &ModelWindow::projectChanged, this, [this] {
            refreshActions();
            if (const int stale = staleViewCount(); stale > 0)
                statusBar()->showMessage(tr("Виды на открытом чертеже устарели: %1. Нажмите «Обновить виды».").arg(stale), 10000);
        });
    }
    return modelWindow_;
}

bool MainWindow::addDrawing(const QString& name) {
    auto created = context_.documents().createDocument(document::DocumentKind::Drawing, name.toStdString());
    if (!created) {
        report(created.error());
        return false;
    }
    drawing::SheetSetup setup;
    setup.profileId = "eskd";
    if (auto profile = context_.standards().profile("eskd")) setup.profileVersion = profile.value().version;
    setup.format = "A4";
    if (auto sheet = context_.drawing().setupSheet(*created.value(), setup); !sheet) report(sheet.error());
    updatingDrawings_ = true;
    const int tab = drawings_->addTab(name);
    drawings_->setTabData(tab, QString::fromStdString(created.value()->id().toString()));
    updatingDrawings_ = false;
    drawings_->setCurrentIndex(drawings_->count() - 1);
    return true;
}

void MainWindow::refreshLayers() {
    updatingLayers_ = true;
    const core::Uuid current = view_->currentLayer();
    layers_->clear();
    auto* none = new QListWidgetItem(tr("(без слоя — основная линия)"), layers_);
    none->setData(Qt::UserRole, QString::fromStdString(core::Uuid().toString()));
    int currentRow = 0;
    if (document::Document* document = view_->document()) {
        auto profile = context_.profileFor(*document);
        for (const auto* layer : document->objectsOf<drawing::Layer>()) {
            QString lineType = QString::fromStdString(layer->lineTypeId);
            if (profile)
                if (const auto* type = profile->findLineType(layer->lineTypeId)) lineType = QString::fromStdString(type->name);
            auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(QString::fromStdString(layer->name), lineType),
                                             layers_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(layer->visible ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, QString::fromStdString(layer->id().toString()));
            if (layer->id() == current) currentRow = layers_->count() - 1;
        }
    }
    layers_->setCurrentRow(currentRow);
    updatingLayers_ = false;
    if (currentRow == 0) view_->setCurrentLayer({});
}

void MainWindow::refreshActions() {
    document::Document* document = view_->document();
    undoAction_->setEnabled(document && document->canUndo());
    redoAction_->setEnabled(document && document->canRedo());
    undoAction_->setText(document && document->canUndo()
                             ? tr("&Отменить: %1").arg(QString::fromStdString(document->undoName()))
                             : tr("&Отменить"));
    redoAction_->setText(document && document->canRedo()
                             ? tr("&Повторить: %1").arg(QString::fromStdString(document->redoName()))
                             : tr("&Повторить"));
    QString title = QString::fromUtf8(core::kAppName);
    if (!projectPath_.empty()) title += QStringLiteral(" — ") + fromPath(projectPath_.filename());
    if (isModified()) title += QStringLiteral(" *");
    setWindowTitle(title);
}

bool MainWindow::isModified() const {
    for (const auto& document : context_.documents().project().documents())
        if (document->isModified()) return true;
    return false;
}

bool MainWindow::confirmDiscard() {
    if (!isModified()) return true;
    auto answer = QMessageBox::question(this, tr("Несохранённые изменения"),
                                        tr("В проекте есть несохранённые изменения. Сохранить их?"),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) {
        onSave();
        return !isModified();
    }
    return true;
}

void MainWindow::newProjectWithDemo() {
    view_->setDocument(nullptr);
    context_.documents().newProject("Пример");
    projectPath_.clear();
    if (auto demo = context_.createDemoDrawing(); !demo)
        QMessageBox::warning(this, tr("Ошибка"), QString::fromStdString(demo.error().message));
    if (auto demo = context_.createDemoModel(); !demo)
        QMessageBox::warning(this, tr("Ошибка"), QString::fromStdString(demo.error().message));
    if (auto demo = context_.createDemoSchematic(); !demo)
        QMessageBox::warning(this, tr("Ошибка"), QString::fromStdString(demo.error().message));
    for (const auto& document : context_.documents().project().documents()) document->markSaved();
    showFirstDrawing();
}

void MainWindow::newEmptyDrawing() {
    view_->setDocument(nullptr);
    context_.documents().newProject("Новый проект");
    projectPath_.clear();
    auto created = context_.documents().createDocument(document::DocumentKind::Drawing, "Чертёж 1");
    if (created) {
        drawing::SheetSetup setup;
        setup.profileId = "eskd";
        if (auto profile = context_.standards().profile("eskd")) setup.profileVersion = profile.value().version;
        setup.format = "A4";
        if (auto sheet = context_.drawing().setupSheet(*created.value(), setup); !sheet) report(sheet.error());
        created.value()->markSaved();
    }
    showFirstDrawing();
}

bool MainWindow::openProject(const fs::path& path) {
    fs::path source = path;
    if (storage::ProjectStorage::hasNewerAutosave(path)) {
        auto answer = QMessageBox::question(
            this, tr("Восстановление"),
            tr("Найдено автосохранение новее сохранённого проекта. Вероятно, программа была закрыта аварийно.\n"
               "Открыть автосохранение?"));
        if (answer == QMessageBox::Yes) source = storage::ProjectStorage::autosavePath(path);
    }
    view_->setDocument(nullptr);
    if (auto opened = context_.documents().openProject(source); !opened) {
        QMessageBox::warning(this, tr("Не удалось открыть проект"), QString::fromStdString(opened.error().message));
        showFirstDrawing();
        return false;
    }
    projectPath_ = path;
    // Тела деталей в файле не хранятся — строим их заново.
    if (auto rebuilt = context_.rebuildAll(); !rebuilt) report(rebuilt.error());
    showFirstDrawing();
    return true;
}

bool MainWindow::saveTo(const fs::path& path) {
    if (auto saved = context_.documents().saveProject(path); !saved) {
        QMessageBox::warning(this, tr("Не удалось сохранить проект"), QString::fromStdString(saved.error().message));
        return false;
    }
    projectPath_ = path;
    refreshActions();
    statusBar()->showMessage(tr("Проект сохранён"), 3000);
    return true;
}

void MainWindow::onNew() {
    if (confirmDiscard()) newEmptyDrawing();
}

void MainWindow::onOpen() {
    if (!confirmDiscard()) return;
    QString directory = QFileDialog::getExistingDirectory(this, tr("Открыть проект (каталог .cosmocad)"));
    if (!directory.isEmpty()) openProject(toPath(directory));
}

void MainWindow::onSave() {
    if (projectPath_.empty()) return onSaveAs();
    saveTo(projectPath_);
}

void MainWindow::onSaveAs() {
    QString name = QFileDialog::getSaveFileName(this, tr("Сохранить проект"), QString(), tr("Проект CosmoCad (*.cosmocad)"));
    if (name.isEmpty()) return;
    if (!name.endsWith(QStringLiteral(".cosmocad"))) name += QStringLiteral(".cosmocad");
    saveTo(toPath(name));
}

void MainWindow::onExport() {
    if (!view_->document()) return;
    QString name = QFileDialog::getSaveFileName(this, tr("Экспорт чертежа"), QString(),
                                                tr("PDF (*.pdf);;SVG (*.svg);;DXF (*.dxf)"));
    if (name.isEmpty()) return;
    const graphics::Scene& scene = view_->currentScene();
    bool ok = false;
    if (name.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        ok = exportPdf(scene, name);
    } else {
        std::ofstream out(toPath(name), std::ios::binary);
        if (out) {
            if (name.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) ok = exchange::writeSvg(scene, out).ok();
            else if (name.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive)) ok = exchange::writeDxf(scene, out).ok();
        }
    }
    statusBar()->showMessage(ok ? tr("Экспорт выполнен") : tr("Экспорт не выполнен: укажите расширение .pdf, .svg или .dxf"),
                             5000);
}

void MainWindow::onUndo() {
    if (view_->document() && view_->document()->undo()) refreshActions();
}

void MainWindow::onRedo() {
    if (view_->document() && view_->document()->redo()) refreshActions();
}

void MainWindow::onAutosave() {
    if (projectPath_.empty() || !isModified()) return;
    if (auto saved = context_.storage().saveAutosave(context_.documents().project(), projectPath_); !saved)
        statusBar()->showMessage(tr("Автосохранение не удалось: %1").arg(QString::fromStdString(saved.error().message)),
                                 5000);
}

// Окно «Лист и основная надпись»: формат, ориентация, толщина основной линии и поля надписи.
// Список форматов и полей берётся из профиля стандарта.
void MainWindow::onSheetSetup() {
    document::Document* document = view_->document();
    if (!document) return;
    auto sheets = document->objectsOf<drawing::Sheet>();
    auto profile = context_.profileFor(*document);
    if (sheets.empty() || !profile) {
        statusBar()->showMessage(tr("В чертеже нет листа или не выбран профиль стандарта"), 5000);
        return;
    }
    const drawing::Sheet& sheet = *sheets.front();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Лист и основная надпись"));
    auto* form = new QFormLayout(&dialog);

    auto* format = new QComboBox(&dialog);
    for (const auto& f : profile->sheetFormats)
        format->addItem(QStringLiteral("%1 (%2 × %3 мм)").arg(QString::fromStdString(f.name)).arg(f.width).arg(f.height),
                        QString::fromStdString(f.name));
    format->setCurrentIndex(std::max(0, format->findData(QString::fromStdString(sheet.format))));
    form->addRow(tr("Формат"), format);

    auto* landscape = new QCheckBox(tr("Альбомная ориентация"), &dialog);
    landscape->setChecked(sheet.landscape);
    form->addRow(QString(), landscape);

    auto* width = new QDoubleSpinBox(&dialog);
    width->setRange(0.1, 5.0);
    width->setSingleStep(0.1);
    width->setDecimals(2);
    width->setSuffix(tr(" мм"));
    width->setValue(sheet.mainLineWidth);
    form->addRow(tr("Толщина основной линии"), width);

    std::vector<std::pair<std::string, QLineEdit*>> edits;
    for (const auto& field : profile->titleBlock.fields) {
        auto* edit = new QLineEdit(&dialog);
        auto it = sheet.titleBlock.find(field.key);
        if (it != sheet.titleBlock.end()) edit->setText(QString::fromStdString(it->second));
        bool required = std::find(profile->titleBlock.requiredFields.begin(), profile->titleBlock.requiredFields.end(),
                                  field.key) != profile->titleBlock.requiredFields.end();
        form->addRow(QString::fromStdString(field.title) + (required ? QStringLiteral(" *") : QString()), edit);
        edits.emplace_back(field.key, edit);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    // Поля, которых нет в профиле, сохраняются как были.
    std::map<std::string, std::string> titleBlock = sheet.titleBlock;
    for (const auto& [key, edit] : edits) titleBlock[key] = edit->text().toStdString();
    auto updated = context_.drawing().updateSheet(*document, format->currentData().toString().toStdString(),
                                                  landscape->isChecked(), width->value(), titleBlock);
    if (!updated) return report(updated.error());
    view_->fitAll();
}

// Спрашивает строку и высоту шрифта из ряда, заданного профилем. Возвращает false при отмене.
bool MainWindow::askTextAndHeight(const QString& title, QString& text, double& height) {
    document::Document* document = view_->document();
    if (!document) return false;
    bool ok = false;
    text = QInputDialog::getText(this, title, tr("Текст:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty()) return false;

    height = 3.5;
    if (auto profile = context_.profileFor(*document); profile && !profile->textHeights.empty()) {
        QStringList heights;
        int preferred = 0;
        for (std::size_t i = 0; i < profile->textHeights.size(); ++i) {
            heights << QString::number(profile->textHeights[i]);
            if (std::abs(profile->textHeights[i] - 3.5) < 1e-9) preferred = static_cast<int>(i);
        }
        QString chosen = QInputDialog::getItem(this, title, tr("Высота шрифта, мм:"), heights, preferred, false, &ok);
        if (!ok) return false;
        height = chosen.toDouble();
    }
    return true;
}

void MainWindow::onTextRequested(double x, double y) {
    QString text;
    double height = 3.5;
    if (askTextAndHeight(tr("Текст"), text, height)) view_->placeText({x, y}, text, height);
}

void MainWindow::onLeaderRequested(double tipX, double tipY, double kneeX, double kneeY) {
    QString text;
    double height = 3.5;
    if (askTextAndHeight(tr("Выноска"), text, height)) view_->placeLeader({tipX, tipY}, {kneeX, kneeY}, text, height);
}

// Печать в масштабе 1:1. Размер бумаги берётся из листа чертежа; изменить его можно в окне печати.
void MainWindow::onPrint() {
    if (!view_->document()) return;
    const graphics::Scene& scene = view_->currentScene();
    QPrinter printer(QPrinter::HighResolution);
    printer.setFullPage(true);  // без полей: иначе чертёж сдвинется и рамка не совпадёт с листом
    if (scene.sheetWidth > 0.0 && scene.sheetHeight > 0.0) {
        bool landscape = scene.sheetWidth > scene.sheetHeight;
        QSizeF portrait(std::min(scene.sheetWidth, scene.sheetHeight), std::max(scene.sheetWidth, scene.sheetHeight));
        printer.setPageSize(QPageSize(portrait, QPageSize::Millimeter));
        printer.setPageOrientation(landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
    }
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Печать чертежа"));
    if (dialog.exec() != QDialog::Accepted) return;
    bool ok = printScene(scene, printer);
    statusBar()->showMessage(ok ? tr("Чертёж отправлен на печать") : tr("Не удалось напечатать"), 5000);
}

// Штриховка выбранного контура: шаг спрашиваем, наклон 45°.
void MainWindow::onHatch() {
    if (!view_->document()) return;
    if (view_->selection().empty()) {
        statusBar()->showMessage(tr("Сначала выберите линии, ограничивающие область (замкнутый контур)"), 6000);
        return;
    }
    bool ok = false;
    double spacing = QInputDialog::getDouble(this, tr("Штриховка"), tr("Расстояние между линиями, мм:"), 3.0, 0.5, 50.0, 1, &ok);
    if (!ok) return;
    view_->hatchSelection(45.0, spacing);
}

void MainWindow::onAddLayer() {
    document::Document* document = view_->document();
    if (!document) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Новый слой"), tr("Имя слоя:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    QStringList titles;
    std::vector<std::string> ids;
    if (auto profile = context_.profileFor(*document)) {
        for (const auto& type : profile->lineTypes) {
            titles << QString::fromStdString(type.name);
            ids.push_back(type.id);
        }
    }
    std::string lineType = "solid_main";
    if (!titles.isEmpty()) {
        QString chosen = QInputDialog::getItem(this, tr("Новый слой"), tr("Тип линии:"), titles, 0, false, &ok);
        if (!ok) return;
        lineType = ids[static_cast<std::size_t>(titles.indexOf(chosen))];
    }
    auto added = context_.drawing().addLayer(*document, name.trimmed().toStdString(), lineType);
    if (!added) return report(added.error());
    view_->setCurrentLayer(added.value());
    refreshLayers();
}

void MainWindow::onMoveToLayer() {
    document::Document* document = view_->document();
    if (!document || view_->selection().empty()) {
        statusBar()->showMessage(tr("Сначала выберите элементы"), 4000);
        return;
    }
    std::vector<core::Uuid> ids(view_->selection().begin(), view_->selection().end());
    if (auto moved = context_.drawing().setEntitiesLayer(*document, ids, view_->currentLayer()); !moved)
        report(moved.error());
}

int MainWindow::runCheck() {
    violations_->clear();
    document::Document* document = view_->document();
    if (!document) return 0;
    auto result = context_.standards().check(*document);
    if (!result) {
        violations_->addItem(QString::fromStdString(result.error().message));
        return 0;
    }
    for (const auto& v : result.value()) {
        QString prefix = v.severity == standards::Severity::Error     ? tr("Ошибка")
                         : v.severity == standards::Severity::Warning ? tr("Предупреждение")
                                                                      : tr("Сведение");
        QString text = QStringLiteral("%1 · %2 · %3").arg(prefix, QString::fromStdString(v.ruleId),
                                                         QString::fromStdString(v.message));
        if (!v.reference.empty()) text += QStringLiteral(" — ") + QString::fromStdString(v.reference);
        auto* item = new QListWidgetItem(text, violations_);
        item->setData(Qt::UserRole, QString::fromStdString(v.objectId.toString()));
    }
    if (result.value().empty()) violations_->addItem(tr("Замечаний нет."));
    if (const int stale = staleViewCount(); stale > 0)
        violations_->addItem(tr("Виды по модели устарели: %1. Модель изменилась — нажмите «Обновить виды» в окне 3D.").arg(stale));
    return static_cast<int>(result.value().size());
}

// ---- Ограничения эскиза ----

void MainWindow::syncPartsWithDrawing() {
    bool changed = false;
    for (const auto& document : context_.documents().project().documents())
        if (document->kind() == document::DocumentKind::Part)
            if (auto synced = context_.syncSketches(*document); synced && synced.value() > 0) changed = true;
    if (changed && modelWindow_) modelWindow_->reloadModel();
}

int MainWindow::constrainSelection(sketcher::ConstraintKind kind, const QString& valueText) {
    bool isNumber = false;
    const double number = QString(valueText).replace(',', '.').trimmed().toDouble(&isNumber);
    if (isNumber) return constrainSelection(kind, number);
    // Формула: параметры берутся из детали, выбранной в окне 3D.
    document::Document* part = modelWindow_ ? modelWindow_->currentDocument() : nullptr;
    if (!part || part->kind() != document::DocumentKind::Part) {
        statusBar()->showMessage(tr("Формула берёт параметры из детали: откройте окно 3D и выберите в нём деталь."), 8000);
        return 0;
    }
    auto value = context_.evaluateWith(*part, valueText.trimmed().toStdString());
    if (!value) {
        report(value.error());
        return 0;
    }
    pendingExpression_ = valueText.trimmed().toStdString();
    pendingParameters_ = part->id();
    const int added = constrainSelection(kind, value.value());
    pendingExpression_.clear();
    pendingParameters_ = core::Uuid();
    return added;
}

int MainWindow::constrainSelection(sketcher::ConstraintKind kind, double value) {
    using sketcher::ConstraintKind;
    using sketcher::PointRole;
    document::Document* document = view_->document();
    if (!document) return 0;
    const std::vector<core::Uuid> ids(view_->selection().begin(), view_->selection().end());
    if (ids.empty()) {
        statusBar()->showMessage(tr("Сначала выберите отрезки, окружности или дуги."), 6000);
        return 0;
    }
    const auto isLine = [&](const core::Uuid& id) { return document->findAs<drawing::LineEntity>(id) != nullptr; };
    std::vector<sketcher::ConstraintParams> list;
    const bool single = kind == ConstraintKind::Fixed || kind == ConstraintKind::Horizontal || kind == ConstraintKind::Vertical ||
                        kind == ConstraintKind::Length || kind == ConstraintKind::Radius;
    if (single) {
        for (const auto& id : ids) list.push_back({kind, {{id, PointRole::Whole}}, value, pendingExpression_, pendingParameters_});
    } else if (kind == ConstraintKind::Symmetric) {
        // Три отрезка: ось — тот, относительно которого два других ближе всего к зеркальным.
        std::vector<const drawing::LineEntity*> lines;
        for (const auto& id : ids)
            if (const auto* line = document->findAs<drawing::LineEntity>(id)) lines.push_back(line);
        if (ids.size() != 3 || lines.size() != 3) {
            statusBar()->showMessage(tr("Для симметрии выберите три отрезка: два зеркальных и ось между ними."), 8000);
            return 0;
        }
        const auto mirror = [](geometry::Point2 p, const drawing::LineEntity& axis) {
            const double dx = axis.end.x - axis.start.x, dy = axis.end.y - axis.start.y;
            const double t = ((p.x - axis.start.x) * dx + (p.y - axis.start.y) * dy) / (dx * dx + dy * dy);
            const geometry::Point2 foot{axis.start.x + t * dx, axis.start.y + t * dy};
            return geometry::Point2{2 * foot.x - p.x, 2 * foot.y - p.y};
        };
        double best = 1e300;
        std::size_t bestAxis = 0;
        bool bestSwapped = false;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto* a = lines[(axis + 1) % 3];
            const auto* b = lines[(axis + 2) % 3];
            const geometry::Point2 ms = mirror(a->start, *lines[axis]), me = mirror(a->end, *lines[axis]);
            const double direct = geometry::distance(ms, b->start) + geometry::distance(me, b->end);
            const double swapped = geometry::distance(ms, b->end) + geometry::distance(me, b->start);
            if (std::min(direct, swapped) < best) {
                best = std::min(direct, swapped);
                bestAxis = axis;
                bestSwapped = swapped < direct;
            }
        }
        const core::Uuid axisId = lines[bestAxis]->id(), a = lines[(bestAxis + 1) % 3]->id(), b = lines[(bestAxis + 2) % 3]->id();
        list.push_back({kind, {{a, PointRole::Start}, {b, bestSwapped ? PointRole::End : PointRole::Start}, {axisId, PointRole::Whole}}, 0.0});
        list.push_back({kind, {{a, PointRole::End}, {b, bestSwapped ? PointRole::Start : PointRole::End}, {axisId, PointRole::Whole}}, 0.0});
    } else {
        if (ids.size() != 2) {
            statusBar()->showMessage(tr("Для этого ограничения выберите ровно два элемента."), 6000);
            return 0;
        }
        // Точки элемента, которые можно связывать: концы отрезка или центр окружности.
        const auto pointsOf = [&](const core::Uuid& id) {
            std::vector<std::pair<PointRole, geometry::Point2>> points;
            if (const auto* line = document->findAs<drawing::LineEntity>(id)) {
                points = {{PointRole::Start, line->start}, {PointRole::End, line->end}};
            } else if (const auto* circle = document->findAs<drawing::CircleEntity>(id)) {
                points = {{PointRole::Center, circle->center}};
            } else if (const auto* arc = document->findAs<drawing::ArcEntity>(id)) {
                points = {{PointRole::Center, arc->center}};
            }
            return points;
        };
        if (kind == ConstraintKind::Distance) {
            // Ближайшая пара точек двух элементов.
            const auto first = pointsOf(ids[0]), second = pointsOf(ids[1]);
            if (first.empty() || second.empty()) {
                statusBar()->showMessage(tr("Расстояние задаётся между отрезками, окружностями или дугами."), 6000);
                return 0;
            }
            double best = 1e300;
            PointRole ra = first[0].first, rb = second[0].first;
            for (const auto& [roleA, pa] : first)
                for (const auto& [roleB, pb] : second)
                    if (geometry::distance(pa, pb) < best) {
                        best = geometry::distance(pa, pb);
                        ra = roleA;
                        rb = roleB;
                    }
            list.push_back({kind, {{ids[0], ra}, {ids[1], rb}}, value, pendingExpression_, pendingParameters_});
        } else if (kind == ConstraintKind::PointOnLine) {
            const auto* a = document->findAs<drawing::LineEntity>(ids[0]);
            const auto* b = document->findAs<drawing::LineEntity>(ids[1]);
            if (!a || !b) {
                statusBar()->showMessage(tr("Для «точки на прямой» выберите два отрезка."), 6000);
                return 0;
            }
            // Из четырёх концов берётся тот, что ближе всего к прямой другого отрезка.
            const auto offLine = [](geometry::Point2 p, const drawing::LineEntity& line) {
                const double dx = line.end.x - line.start.x, dy = line.end.y - line.start.y;
                return std::abs((p.x - line.start.x) * dy - (p.y - line.start.y) * dx) / std::hypot(dx, dy);
            };
            struct Option {
                double error;
                core::Uuid pointOwner, line;
                PointRole role;
            };
            const Option options[] = {{offLine(a->start, *b), ids[0], ids[1], PointRole::Start}, {offLine(a->end, *b), ids[0], ids[1], PointRole::End},
                                      {offLine(b->start, *a), ids[1], ids[0], PointRole::Start}, {offLine(b->end, *a), ids[1], ids[0], PointRole::End}};
            const Option* chosen = &options[0];
            for (const auto& option : options)
                if (option.error < chosen->error) chosen = &option;
            list.push_back({kind, {{chosen->pointOwner, chosen->role}, {chosen->line, PointRole::Whole}}, 0.0});
        } else if (kind == ConstraintKind::Coincident) {
            const auto* a = document->findAs<drawing::LineEntity>(ids[0]);
            const auto* b = document->findAs<drawing::LineEntity>(ids[1]);
            if (!a || !b) {
                statusBar()->showMessage(tr("Соединять концы можно у двух отрезков."), 6000);
                return 0;
            }
            // Совмещаются ближайшие друг к другу концы.
            PointRole bestA = PointRole::Start, bestB = PointRole::Start;
            double best = 1e300;
            for (const PointRole ra : {PointRole::Start, PointRole::End})
                for (const PointRole rb : {PointRole::Start, PointRole::End}) {
                    const double d = geometry::distance(ra == PointRole::Start ? a->start : a->end, rb == PointRole::Start ? b->start : b->end);
                    if (d < best) {
                        best = d;
                        bestA = ra;
                        bestB = rb;
                    }
                }
            list.push_back({kind, {{ids[0], bestA}, {ids[1], bestB}}, 0.0});
        } else {
            // «Равны» для двух окружностей — равные радиусы.
            ConstraintKind actual = kind;
            if (kind == ConstraintKind::EqualLength && !isLine(ids[0]) && !isLine(ids[1])) actual = ConstraintKind::EqualRadius;
            list.push_back({actual, {{ids[0], PointRole::Whole}, {ids[1], PointRole::Whole}}, value, pendingExpression_, pendingParameters_});
        }
    }
    int added = 0;
    for (const auto& params : list) {
        auto id = context_.sketcher().addConstraint(*document, params);
        if (!id) {
            report(id.error());
            break;
        }
        ++added;
    }
    view_->update();
    refreshActions();
    if (added == static_cast<int>(list.size())) statusBar()->showMessage(tr("Ограничений поставлено: %1. Фигура подогнана.").arg(added), 6000);
    if (added > 0) syncPartsWithDrawing();
    return added;
}

int MainWindow::removeConstraintsOfSelection() {
    document::Document* document = view_->document();
    if (!document) return 0;
    std::vector<core::Uuid> doomed;
    for (const auto& id : view_->selection())
        for (const auto* constraint : context_.sketcher().constraintsOf(*document, id))
            if (std::find(doomed.begin(), doomed.end(), constraint->id()) == doomed.end()) doomed.push_back(constraint->id());
    for (const auto& id : doomed)
        if (auto ok = context_.sketcher().removeConstraint(*document, id); !ok) report(ok.error());
    statusBar()->showMessage(tr("Снято ограничений: %1").arg(doomed.size()), 6000);
    refreshActions();
    return static_cast<int>(doomed.size());
}

bool MainWindow::setConstraintValueOfSelection(double value) {
    using sketcher::ConstraintKind;
    document::Document* document = view_->document();
    if (!document) return false;
    for (const auto& id : view_->selection())
        for (const auto* constraint : context_.sketcher().constraintsOf(*document, id))
            if (constraint->kind == ConstraintKind::Length || constraint->kind == ConstraintKind::Radius) {
                auto changed = context_.sketcher().setValue(*document, constraint->id(), value);
                if (!changed) {
                    report(changed.error());
                    return false;
                }
                view_->update();
                refreshActions();
                syncPartsWithDrawing();
                return true;
            }
    statusBar()->showMessage(tr("У выбранного элемента нет размера «Длина» или «Радиус»."), 6000);
    return false;
}

bool MainWindow::solveConstraints() {
    document::Document* document = view_->document();
    if (!document) return false;
    (void)context_.sketcher().removeOrphans(*document);
    auto solved = context_.sketcher().solve(*document);
    if (!solved) {
        report(solved.error());
        return false;
    }
    view_->update();
    refreshActions();
    statusBar()->showMessage(solved.value().solved ? tr("Фигура подогнана; элементов сдвинуто: %1").arg(solved.value().movedEntities)
                                                    : tr("Ограничения противоречат друг другу: фигура не тронута."), 6000);
    return solved.value().solved;
}

QString MainWindow::constraintsText() {
    document::Document* document = view_->document();
    if (!document) return QString();
    const auto constraints = document->objectsOf<sketcher::Constraint>();
    if (constraints.empty()) return tr("На чертеже нет ограничений.");
    auto report = context_.sketcher().analyze(*document);
    if (!report) return tr("Не выполнено: ") + QString::fromStdString(report.error().message);
    QStringList lines;
    switch (report.value().state()) {
        case sketcher::SketchState::FullyDefined: lines << tr("Эскиз определён полностью: свободных движений нет."); break;
        case sketcher::SketchState::UnderDefined:
            lines << tr("Эскиз недоопределён: свободных движений — %1.").arg(report.value().freedom);
            break;
        case sketcher::SketchState::OverDefined:
            lines << (report.value().solved ? tr("Эскиз переопределён: лишних условий — %1.").arg(report.value().redundant)
                                            : tr("Эскиз переопределён: ограничения противоречат друг другу."));
            break;
        case sketcher::SketchState::Empty: break;
    }
    if (!report.value().satisfied) lines << tr("Сейчас ограничения нарушены — нажмите «Пересчитать».");
    lines << QString();
    for (const auto* constraint : constraints) {
        QString line = QString::fromStdString(constraint->name);
        using sketcher::ConstraintKind;
        if (constraint->kind == ConstraintKind::Length || constraint->kind == ConstraintKind::Radius || constraint->kind == ConstraintKind::Distance)
            line += tr(" = %1 мм").arg(QLocale(QLocale::Russian).toString(constraint->value, 'g', 8));
        if (constraint->kind == ConstraintKind::Angle) line += tr(" = %1°").arg(QLocale(QLocale::Russian).toString(constraint->value, 'g', 8));
        if (std::find(report.value().unsatisfied.begin(), report.value().unsatisfied.end(), constraint->id()) != report.value().unsatisfied.end())
            line += tr(" — не выполняется");
        lines << line;
    }
    return lines.join("\n");
}

bool MainWindow::runPluginCommand(const QString& commandId) {
    auto result = context_.plugins().run(commandId.toStdString());
    if (!result) {
        report(result.error());
        return false;
    }
    view_->update();
    refreshActions();
    lastPluginResult_ = QString::fromStdString(result.value());
    // Короткий ответ — в строку состояния, длинный (таблица) — в окно.
    if (lastPluginResult_.count('\n') > 1)
        QMessageBox::information(this, tr("Дополнение"), lastPluginResult_);
    else
        statusBar()->showMessage(lastPluginResult_, 10000);
    return true;
}

bool MainWindow::setPluginEnabled(const QString& pluginId, bool enabled) {
    auto changed = context_.setPluginDisabled(pluginId.toStdString(), !enabled);
    if (!changed) {
        report(changed.error());
        return false;
    }
    statusBar()->showMessage(enabled ? tr("Дополнение будет загружено при следующем запуске программы.")
                                     : tr("Дополнение не будет загружаться со следующего запуска программы."), 10000);
    return true;
}

QString MainWindow::pluginsText() {
    const auto& plugins = context_.plugins().plugins();
    if (plugins.empty()) return tr("Дополнений не найдено. Папка для них — «plugins» рядом с программой или в папке пользователя.");
    QStringList lines;
    for (const auto& plugin : plugins) {
        QString state;
        switch (plugin.state) {
            case app::PluginState::Active: state = tr("работает"); break;
            case app::PluginState::Disabled: state = tr("выключено"); break;
            case app::PluginState::Rejected: state = tr("отклонено"); break;
            case app::PluginState::Failed: state = tr("сбой"); break;
            case app::PluginState::Found: state = tr("найдено"); break;
        }
        QString line = QStringLiteral("%1 %2 — %3").arg(QString::fromStdString(plugin.name), QString::fromStdString(plugin.version), state);
        if (!plugin.reason.empty()) line += QStringLiteral(": ") + QString::fromStdString(plugin.reason);
        lines << line;
    }
    return lines.join("\n");
}

bool MainWindow::makeBlockFromSelection(const QString& name) {
    document::Document* document = view_->document();
    if (!document) return false;
    const std::vector<core::Uuid> ids(view_->selection().begin(), view_->selection().end());
    geometry::BBox2 box;
    for (const auto& id : ids)
        if (const auto* entity = document->findAs<drawing::Entity>(id)) box.expand(entity->bounds());
    if (!box.valid) {
        statusBar()->showMessage(tr("Сначала выберите элементы для блока."), 6000);
        return false;
    }
    auto made = context_.drawing().createBlock(*document, name.toStdString(), ids, box.min, true);
    if (!made) {
        report(made.error());
        return false;
    }
    view_->clearSelection();
    view_->update();
    refreshActions();
    statusBar()->showMessage(tr("Блок «%1» создан; выбранные элементы заменены его вставкой.").arg(name), 6000);
    return true;
}

bool MainWindow::insertBlock(const QString& name, double x, double y, double angleDegrees) {
    document::Document* document = view_->document();
    if (!document) return false;
    auto inserted = context_.drawing().insertBlock(*document, name.toStdString(), {x, y}, angleDegrees);
    if (!inserted) {
        report(inserted.error());
        return false;
    }
    view_->update();
    refreshActions();
    return true;
}

bool MainWindow::explodeSelectedBlock() {
    document::Document* document = view_->document();
    if (!document) return false;
    const std::vector<core::Uuid> ids(view_->selection().begin(), view_->selection().end());
    int exploded = 0;
    for (const auto& id : ids)
        if (document->findAs<drawing::BlockReference>(id)) {
            auto parts = context_.drawing().explodeBlock(*document, id);
            if (!parts) {
                report(parts.error());
                return false;
            }
            ++exploded;
        }
    if (exploded == 0) {
        statusBar()->showMessage(tr("Выберите вставку блока."), 6000);
        return false;
    }
    view_->clearSelection();
    view_->update();
    refreshActions();
    return true;
}

bool MainWindow::setTechnicalRequirements(const QStringList& items) {
    document::Document* document = view_->document();
    if (!document) return false;
    std::vector<std::string> list;
    for (const QString& item : items) list.push_back(item.toStdString());
    auto done = context_.setTechnicalRequirements(*document, list);
    if (!done) {
        report(done.error());
        return false;
    }
    view_->update();
    refreshActions();
    return true;
}

bool MainWindow::setSheetRoughness(const QString& parameter, int kind, bool others) {
    document::Document* document = view_->document();
    if (!document) return false;
    const auto signKind = kind == 1 ? drawing::RoughnessSign::Kind::Removal
                          : kind == 2 ? drawing::RoughnessSign::Kind::NoRemoval : drawing::RoughnessSign::Kind::Any;
    auto done = context_.setSheetRoughness(*document, parameter.trimmed().toStdString(), signKind, others);
    if (!done) {
        report(done.error());
        return false;
    }
    view_->update();
    refreshActions();
    return true;
}

int MainWindow::staleViewCount() {
    document::Document* document = view_->document();
    if (!document) return 0;
    auto stale = context_.staleViewCount(*document);
    return stale ? stale.value() : 0;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (confirmDiscard()) {
        view_->setDocument(nullptr);
        event->accept();
    } else {
        event->ignore();
    }
}

// ---- Вид: кнопки поверх чертежа держатся у правого края ----

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show) && viewButtons_)
        viewButtons_->move(view_->width() - viewButtons_->width() - 12, (view_->height() - viewButtons_->height()) / 2);
    return QMainWindow::eventFilter(watched, event);
}

// ---- Электросхема ----

bool MainWindow::placeSchematicElement(const QString& symbolId, geometry::Point2 at, int rotation, const QString& value) {
    document::Document* document = view_->document();
    if (!document) return false;
    // Место выравнивается по шагу 5 мм: выводы соседних элементов оказываются на одной линии.
    const geometry::Point2 snapped{std::round(at.x / 5.0) * 5.0, std::round(at.y / 5.0) * 5.0};
    auto added = context_.schematic().addElement(*document, symbolId.toStdString(), snapped, rotation, value.toStdString());
    if (!added) {
        report(added.error());
        return false;
    }
    refreshActions();
    return true;
}

bool MainWindow::placeSchematicWire(geometry::Point2 from, geometry::Point2 to) {
    document::Document* document = view_->document();
    if (!document) return false;
    auto added = context_.schematic().addWire(*document, {from, to});
    if (!added) {
        statusBar()->showMessage(QString::fromStdString(added.error().message), 6000);
        return false;
    }
    refreshActions();
    return true;
}

bool MainWindow::placeSchematicLabel(geometry::Point2 at, const QString& text) {
    document::Document* document = view_->document();
    if (!document) return false;
    auto added = context_.schematic().addLabel(*document, at, text.toStdString());
    if (!added) {
        report(added.error());
        return false;
    }
    refreshActions();
    return true;
}

int MainWindow::renumberSchematic() {
    document::Document* document = view_->document();
    if (!document) return 0;
    auto changed = context_.schematic().renumber(*document);
    if (!changed) {
        report(changed.error());
        return 0;
    }
    statusBar()->showMessage(tr("Изменено обозначений: %1").arg(changed.value()), 6000);
    refreshActions();
    return changed.value();
}

int MainWindow::numberSchematicNets() {
    document::Document* document = view_->document();
    if (!document) return 0;
    auto added = context_.schematic().numberNets(*document);
    if (!added) {
        report(added.error());
        return 0;
    }
    statusBar()->showMessage(added.value() > 0 ? tr("Поставлено номеров цепей: %1").arg(added.value())
                                               : tr("Все цепи уже с именами или номерами."), 6000);
    view_->update();
    refreshActions();
    return added.value();
}

int MainWindow::runSchematicCheck() {
    violations_->clear();
    document::Document* document = view_->document();
    if (!document) return 0;
    if (auto refreshed = context_.schematic().refreshJunctions(*document); !refreshed) report(refreshed.error());
    const auto issues = context_.schematic().check(*document);
    for (const auto& issue : issues) {
        auto* item = new QListWidgetItem(tr("Схема: ") + QString::fromStdString(issue.message), violations_);
        item->setData(Qt::UserRole, QString::fromStdString(issue.objectId.toString()));
    }
    if (issues.empty()) violations_->addItem(tr("Схема: замечаний нет."));
    return static_cast<int>(issues.size());
}

QString MainWindow::schematicNetsText() {
    document::Document* document = view_->document();
    if (!document) return tr("Нет открытого чертежа.");
    const auto nets = context_.schematic().nets(*document);
    if (nets.empty()) return tr("На этом листе нет цепей.");
    QStringList lines;
    for (const auto& net : nets) {
        QStringList pins;
        for (const auto& pin : net.pins) pins << QString::fromStdString(pin.designator + ":" + pin.pin);
        lines << tr("Цепь %1%2 — %3")
                     .arg(net.number)
                     .arg(net.name.empty() ? QString() : " «" + QString::fromStdString(net.name) + "»")
                     .arg(pins.isEmpty() ? tr("без выводов") : pins.join(", "));
    }
    return lines.join("\n");
}

bool MainWindow::exportElementList(const std::filesystem::path& path) {
    document::Document* document = view_->document();
    if (!document) return false;
    if (auto saved = context_.schematic().exportElementListCsv(*document, path); !saved) {
        report(saved.error());
        return false;
    }
    statusBar()->showMessage(tr("Перечень элементов сохранён."), 6000);
    return true;
}

int MainWindow::placeElementListOnSheet() {
    document::Document* document = view_->document();
    if (!document) return 0;
    auto placed = context_.placeElementListTable(*document);
    if (!placed) {
        report(placed.error());
        return 0;
    }
    statusBar()->showMessage(tr("Перечень элементов поставлен на лист, строк: %1").arg(placed.value()), 6000);
    refreshActions();
    return placed.value();
}

bool MainWindow::setSchematicValue(const QString& value) {
    document::Document* document = view_->document();
    if (!document || view_->selection().size() != 1) return false;
    const core::Uuid id = *view_->selection().begin();
    if (!document->findAs<schematic::Element>(id)) return false;
    auto changed = context_.schematic().setValue(*document, id, value.toStdString());
    if (!changed) report(changed.error());
    refreshActions();
    return changed.ok();
}

bool MainWindow::shiftSchematicElements(double dx, double dy) {
    document::Document* document = view_->document();
    if (!document) return false;
    std::vector<core::Uuid> ids;
    for (const auto& id : view_->selection())
        if (document->findAs<schematic::Element>(id)) ids.push_back(id);
    if (ids.empty()) {
        statusBar()->showMessage(tr("Выберите элементы схемы."), 6000);
        return false;
    }
    auto moved = context_.schematic().moveElements(*document, ids, {dx, dy});
    if (!moved) report(moved.error());
    refreshActions();
    return moved.ok();
}

void MainWindow::onSchematicValue() {
    document::Document* document = view_->document();
    if (!document || view_->selection().size() != 1 || !document->findAs<schematic::Element>(*view_->selection().begin())) {
        statusBar()->showMessage(tr("Выберите один элемент схемы."), 6000);
        return;
    }
    const auto* element = document->findAs<schematic::Element>(*view_->selection().begin());
    bool ok = false;
    const QString value = QInputDialog::getText(this, tr("Номинал"), QString::fromStdString(element->designator) + ":",
                                                QLineEdit::Normal, QString::fromStdString(element->value), &ok);
    if (ok) setSchematicValue(value);
}

void MainWindow::onSchematicShift() {
    bool ok = false;
    const double dx = QInputDialog::getDouble(this, tr("Сдвинуть элементы"), tr("Вправо, мм (влево — с минусом):"), 0, -5000, 5000, 1, &ok);
    if (!ok) return;
    const double dy = QInputDialog::getDouble(this, tr("Сдвинуть элементы"), tr("Вверх, мм (вниз — с минусом):"), 0, -5000, 5000, 1, &ok);
    if (ok) shiftSchematicElements(dx, dy);
}

}  // namespace cosmocad::ui
