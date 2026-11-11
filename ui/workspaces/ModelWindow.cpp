#include "ModelWindow.h"

#include <QPushButton>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <cmath>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "Ribbon.h"
#include "cosmocad/modeling/Expression.h"

namespace cosmocad::ui {

using core::Uuid;
using document::Document;
using document::DocumentKind;

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

QString number(double value, int decimals = 2) { return QLocale(QLocale::Russian).toString(value, 'f', decimals); }

Uuid idOf(const QListWidgetItem* item) {
    auto id = Uuid::parse(item->data(Qt::UserRole).toString().toStdString());
    return id ? id.value() : Uuid();
}

}  // namespace

ModelWindow::ModelWindow(app::AppContext& context, std::function<Document*()> currentDrawing,
                         std::function<std::vector<Uuid>()> drawingSelection, QWidget* parent)
    : QMainWindow(parent),
      context_(context),
      currentDrawing_(std::move(currentDrawing)),
      drawingSelection_(std::move(drawingSelection)) {
    setWindowTitle(tr("CosmoCad — 3D-модель"));
    resize(1100, 720);
    buildUi();
    refresh();
}

void ModelWindow::buildUi() {
    view_ = new ModelView3D(this);
    setCentralWidget(view_);
    connect(view_, &ModelView3D::pickedChanged, this, &ModelWindow::onPickedChanged);

    auto* side = new QWidget(this);
    auto* layout = new QVBoxLayout(side);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(new QLabel(tr("Документ:"), side));
    documents_ = new QComboBox(side);
    documents_->setObjectName("model_documents");
    layout->addWidget(documents_);
    connect(documents_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        view_->clearPicked();
        view_->setBodies({});
        rebuild();
    });
    layout->addWidget(new QLabel(tr("Параметры (имя, формула, значение):"), side));
    parameters_ = new QTableWidget(0, 3, side);
    parameters_->setObjectName("model_parameters");
    parameters_->setHorizontalHeaderLabels({tr("Имя"), tr("Формула"), tr("Значение")});
    parameters_->horizontalHeader()->setStretchLastSection(true);
    parameters_->verticalHeader()->hide();
    layout->addWidget(parameters_, 2);
    connect(parameters_, &QTableWidget::cellChanged, this, &ModelWindow::onParameterEdited);
    layout->addWidget(new QLabel(tr("Дерево построения (галочка — операция включена):"), side));
    tree_ = new QListWidget(side);
    tree_->setObjectName("model_tree");
    layout->addWidget(tree_, 3);
    connect(tree_, &QListWidget::itemChanged, this, &ModelWindow::onTreeItemChanged);
    connect(tree_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        Document* document = currentDocument();
        if (!document || document->kind() != DocumentKind::Part) return;
        const auto* feature = document->findAs<modeling::Feature>(idOf(item));
        if (!feature) return;
        bool ok = false;
        const QString value = QInputDialog::getText(this, tr("Размер операции"), tr("Число или формула:"),
                                                    QLineEdit::Normal, text(feature->value), &ok);
        if (ok) setFeatureValue(tree_->row(item), value);
    });
    info_ = new QLabel(side);
    info_->setObjectName("model_info");
    info_->setWordWrap(true);
    layout->addWidget(info_);
    pickedLabel_ = new QLabel(side);
    pickedLabel_->setWordWrap(true);
    layout->addWidget(pickedLabel_);

    auto* dock = new QDockWidget(tr("Модель"), this);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(side);
    dock->setMinimumWidth(330);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    messages_ = new QLabel(this);
    messages_->setObjectName("model_messages");
    statusBar()->addWidget(messages_, 1);

    const auto action = [this](RibbonGroup* bar, const QString& title, const char* name, const QString& tip, auto handler) {
        QAction* a = bar->addAction(title);
        a->setObjectName(name);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, handler);
        return a;
    };
    const auto askText = [this](const QString& title, const QString& label, const QString& initial, QString& out) {
        bool ok = false;
        out = QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &ok);
        return ok && !out.trimmed().isEmpty();
    };

    // Лента команд, как в главном окне.
    setStyleSheet(applicationStyle());
    auto* ribbon = new Ribbon(this);
    ribbon->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QToolBar* ribbonBar = addToolBar(tr("Лента"));
    ribbonBar->setMovable(false);
    ribbonBar->addWidget(ribbon);
    const int modelPage = ribbon->addPage(tr("Модель"));
    const int partPage = ribbon->addPage(tr("Деталь"));
    const int assemblyPage = ribbon->addPage(tr("Сборка"));
    RibbonGroup* documentBar = ribbon->addGroup(modelPage, tr("Документ"));
    action(documentBar, tr("Новая деталь"), "act_new_part", tr("Создать в проекте пустую деталь"), [=, this] {
        QString name;
        if (askText(tr("Новая деталь"), tr("Название:"), tr("Деталь"), name)) newPart(name);
    });
    action(documentBar, tr("Новая сборка"), "act_new_assembly", tr("Создать в проекте пустую сборку"), [=, this] {
        QString name;
        if (askText(tr("Новая сборка"), tr("Название:"), tr("Сборка"), name)) newAssembly(name);
    });
    action(documentBar, tr("Отменить"), "act_model_undo", tr("Отменить последнее действие в этом документе"), [this] { undo(); });
    action(documentBar, tr("Повторить"), "act_model_redo", tr("Повторить отменённое действие"), [this] { redo(); });
    action(documentBar, tr("Удалить"), "act_model_remove", tr("Удалить строку, выбранную в дереве"), [this] { removeSelected(); });
    action(documentBar, tr("STEP…"), "act_export_step", tr("Сохранить тела в файл STEP"), [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Экспорт STEP"), QString(), tr("STEP (*.step *.stp)"));
        if (!path.isEmpty()) exportStep(std::filesystem::path(path.toStdU16String()));
    });
    action(documentBar, tr("STL…"), "act_export_stl", tr("Сохранить деталь в файл STL (для 3D-печати)"), [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Экспорт STL"), QString(), tr("STL (*.stl)"));
        if (!path.isEmpty()) exportStl(std::filesystem::path(path.toStdU16String()));
    });

    RibbonGroup* partBar = ribbon->addGroup(partPage, tr("Построение"));
    action(partBar, tr("Параметр…"), "act_add_parameter", tr("Добавить именованный размер"), [=, this] {
        QString name, value;
        if (askText(tr("Новый параметр"), tr("Имя (например: ширина):"), QString(), name) &&
            askText(tr("Новый параметр"), tr("Число или формула:"), "10", value))
            addParameter(name, value);
    });
    const auto extrude = [=, this](bool cut) {
        QString distance, offset;
        if (askText(cut ? tr("Вырез") : tr("Выдавливание"), tr("На сколько вытянуть контур, мм (число или формула):"),
                    "10", distance) &&
            askText(cut ? tr("Вырез") : tr("Выдавливание"), tr("Высота плоскости эскиза над плоскостью XY, мм:"), "0",
                    offset))
            extrudeContour(distance, offset, cut);
    };
    action(partBar, tr("Выдавить контур"), "act_extrude",
           tr("Выберите на чертеже замкнутый контур (отрезки, окружности) и нажмите эту кнопку"),
           [=] { extrude(false); });
    action(partBar, tr("Протяжка…"), "act_sweep",
           tr("Провести выбранный на чертеже контур вдоль траектории: пруток, труба, гнутый профиль"), [=, this] {
        QString path, radius;
        if (!askText(tr("Протяжка"),
                     tr("Точки траектории после начальной, через «;». В каждой точке три числа через пробел: x y z.\n"
                        "Траектория начинается в середине контура и первым участком уходит вверх. Пример: 0 0 50; 60 0 50"),
                     "0 0 50", path))
            return;
        if (!askText(tr("Протяжка"), tr("Радиус гиба в изломах траектории, мм (для прямой — 0):"), "0", radius)) return;
        sweepContour(path, radius, false);
    });
    action(partBar, tr("Вырезать контур"), "act_cut", tr("То же, но контур вычитается из тела"), [=] { extrude(true); });
    const auto blend = [=, this](bool chamfer) {
        QString size;
        if (askText(chamfer ? tr("Фаска") : tr("Скругление"), tr("Размер, мм (число или формула):"), "2", size))
            blendPicked(size, chamfer);
    };
    action(partBar, tr("Скруглить ребро"), "act_fillet", tr("Щёлкните две грани, между которыми лежит ребро, затем эту кнопку"),
           [=] { blend(false); });
    action(partBar, tr("Фаска"), "act_chamfer", tr("Щёлкните две грани, между которыми лежит ребро, затем эту кнопку"),
           [=] { blend(true); });
    action(partBar, tr("На грани…"), "act_on_face",
           tr("Отверстие или выступ на грани: щёлкните плоскую грань в нужном месте, затем эту кнопку"), [this] {
        const auto at = pickedPointOnFace();
        if (!at) {
            fail(tr("щёлкните по одной плоской грани — в том месте, где нужен элемент"));
            return;
        }
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Элемент на грани"));
        auto* form = new QFormLayout(&dialog);
        auto* mode = new QComboBox(&dialog);
        mode->addItems({tr("Вырезать (отверстие, паз)"), tr("Добавить (бобышка, выступ)")});
        auto* shape = new QComboBox(&dialog);
        shape->addItems({tr("Окружность"), tr("Прямоугольник")});
        const auto rounded = [](double v) { return QString::number(std::round(v * 2.0) / 2.0); };  // до 0,5 мм
        auto* x = new QLineEdit(rounded(at->x), &dialog);
        auto* y = new QLineEdit(rounded(at->y), &dialog);
        auto* size1 = new QLineEdit("10", &dialog);
        auto* size2 = new QLineEdit("10", &dialog);
        auto* depth = new QLineEdit("5", &dialog);
        auto* size1Label = new QLabel(tr("Диаметр, мм:"), &dialog);
        auto* size2Label = new QLabel(tr("Высота, мм:"), &dialog);
        form->addRow(tr("Что сделать:"), mode);
        form->addRow(tr("Фигура:"), shape);
        form->addRow(tr("Середина, X на грани, мм:"), x);
        form->addRow(tr("Середина, Y на грани, мм:"), y);
        form->addRow(size1Label, size1);
        form->addRow(size2Label, size2);
        form->addRow(tr("Глубина (высота), мм:"), depth);
        const auto sync = [=] {
            const bool circle = shape->currentIndex() == 0;
            size1Label->setText(circle ? tr("Диаметр, мм:") : tr("Ширина, мм:"));
            size2Label->setVisible(!circle);
            size2->setVisible(!circle);
        };
        connect(shape, &QComboBox::currentIndexChanged, &dialog, sync);
        sync();
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            addShapeOnPickedFace(shape->currentIndex() == 0, x->text(), y->text(), size1->text(), size2->text(),
                                 depth->text(), mode->currentIndex() == 0);
    });
    action(partBar, tr("Размеры эскиза…"), "act_sketch_values",
           tr("Изменить размеры фигур эскиза у операции, выбранной в дереве"), [this] {
        const int row = tree_->currentRow();
        const QStringList labels = sketchValueLabels(row), values = sketchValues(row);
        if (labels.isEmpty()) {
            fail(tr("выберите в дереве операцию, у которой есть эскиз (выдавливание, вырез, вращение)"));
            return;
        }
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Размеры эскиза"));
        auto* form = new QFormLayout(&dialog);
        std::vector<QLineEdit*> edits;
        for (int i = 0; i < labels.size(); ++i) {
            edits.push_back(new QLineEdit(values.at(i), &dialog));
            form->addRow(labels.at(i), edits.back());
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        // Номер фигуры и размера зашит в порядке строк: см. sketchValueLabels.
        Document* document = currentDocument();
        const auto* feature = document ? document->findAs<modeling::Feature>(idOf(tree_->item(row))) : nullptr;
        const auto* sketch = feature ? document->findAs<modeling::Sketch>(feature->sketchId) : nullptr;
        if (!sketch) return;
        int index = 0;
        for (std::size_t shapeIndex = 0; shapeIndex < sketch->shapes.size(); ++shapeIndex) {
            const std::size_t count = sketch->shapes[shapeIndex].values.size();
            for (std::size_t valueIndex = 0; valueIndex < count; ++valueIndex, ++index)
                if (edits[static_cast<std::size_t>(index)]->text() != values.at(index))
                    if (!setSketchValue(row, static_cast<int>(shapeIndex), static_cast<int>(valueIndex),
                                        edits[static_cast<std::size_t>(index)]->text()))
                        return;
        }
    });
    action(partBar, tr("Массив…"), "act_pattern",
           tr("Повторить выдавливание или вырез, выбранный в дереве, в ряд или по кругу"), [this] {
        Document* document = currentDocument();
        const int row = tree_->currentRow();
        const auto* feature = document && row >= 0 ? document->findAs<modeling::Feature>(idOf(tree_->item(row))) : nullptr;
        if (!feature || feature->kind != modeling::FeatureKind::Extrude) {
            fail(tr("выберите в дереве выдавливание или вырез"));
            return;
        }
        const modeling::Pattern& now = feature->pattern;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Массив"));
        auto* form = new QFormLayout(&dialog);
        auto* kind = new QComboBox(&dialog);
        kind->addItems({tr("Нет массива"), tr("В ряд"), tr("По кругу")});
        kind->setCurrentIndex(static_cast<int>(now.kind));
        auto* count = new QLineEdit(text(now.count == "1" ? std::string("3") : now.count), &dialog);
        const bool circular = now.kind == modeling::Pattern::Kind::Circular;
        auto* a = new QLineEdit(text(circular ? now.centerX : now.stepX), &dialog);
        auto* b = new QLineEdit(text(circular ? now.centerY : now.stepY), &dialog);
        auto* angle = new QLineEdit(text(now.angle), &dialog);
        auto* aLabel = new QLabel(&dialog);
        auto* bLabel = new QLabel(&dialog);
        auto* angleLabel = new QLabel(tr("Угол, на котором разложить копии, градусы:"), &dialog);
        form->addRow(tr("Вид массива:"), kind);
        form->addRow(tr("Число копий вместе с исходной:"), count);
        form->addRow(aLabel, a);
        form->addRow(bLabel, b);
        form->addRow(angleLabel, angle);
        const auto sync = [=] {
            const bool ring = kind->currentIndex() == 2;
            aLabel->setText(ring ? tr("Центр круга, X на эскизе, мм:") : tr("Шаг по X эскиза, мм:"));
            bLabel->setText(ring ? tr("Центр круга, Y на эскизе, мм:") : tr("Шаг по Y эскиза, мм:"));
            angleLabel->setVisible(ring);
            angle->setVisible(ring);
        };
        connect(kind, &QComboBox::currentIndexChanged, &dialog, sync);
        sync();
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            setPattern(row, kind->currentIndex(), count->text(), a->text(), b->text(), angle->text());
    });
    action(partBar, tr("Материал…"), "act_material", tr("Выбрать материал детали из библиотеки или добавить свой"), [this] {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Материал детали"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* filter = new QLineEdit(&dialog);
        filter->setPlaceholderText(tr("Поиск: часть названия или стандарта"));
        auto* list = new QListWidget(&dialog);
        auto* details = new QLabel(&dialog);
        details->setWordWrap(true);
        layout->addWidget(filter);
        layout->addWidget(list);
        layout->addWidget(details);
        const auto fill = [&] {
            list->clear();
            for (const auto& material : context_.materials().find(filter->text().toStdString())) {
                auto* item = new QListWidgetItem(text(material.title()) + tr(" — %1 г/см³").arg(number(material.density, 3)) +
                                                     (material.user ? tr(" (свой)") : QString()),
                                                 list);
                item->setData(Qt::UserRole, text(material.title()));
                item->setToolTip(text(material.note));
            }
        };
        fill();
        connect(filter, &QLineEdit::textChanged, &dialog, fill);
        connect(list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem* item) { details->setText(item ? item->toolTip() : QString()); });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QPushButton* own = buttons->addButton(tr("Свой материал…"), QDialogButtonBox::ActionRole);
        QPushButton* importButton = buttons->addButton(tr("Импорт…"), QDialogButtonBox::ActionRole);
        QPushButton* exportButton = buttons->addButton(tr("Экспорт своих…"), QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(own, &QPushButton::clicked, &dialog, [&] {
            bool ok = false;
            const QString name = QInputDialog::getText(&dialog, tr("Свой материал"), tr("Название (марка):"), QLineEdit::Normal, QString(), &ok);
            if (!ok || name.trimmed().isEmpty()) return;
            const QString standard = QInputDialog::getText(&dialog, tr("Свой материал"), tr("Стандарт (можно оставить пустым):"), QLineEdit::Normal, QString(), &ok);
            if (!ok) return;
            const double density = QInputDialog::getDouble(&dialog, tr("Свой материал"), tr("Плотность, г/см³:"), 7.85, 0.01, 30, 3, &ok);
            if (!ok) return;
            const QString note = QInputDialog::getText(&dialog, tr("Свой материал"), tr("Откуда взята плотность:"), QLineEdit::Normal, QString(), &ok);
            if (ok && addUserMaterial(name, standard, density, note)) fill();
        });
        connect(importButton, &QPushButton::clicked, &dialog, [&] {
            const QString path = QFileDialog::getOpenFileName(&dialog, tr("Импорт материалов"), QString(), tr("Библиотека материалов (*.json)"));
            if (path.isEmpty()) return;
            auto count = context_.materials().importUser(std::filesystem::path(path.toStdU16String()));
            if (!count) {
                fail(count.error());
                return;
            }
            fill();
        });
        connect(exportButton, &QPushButton::clicked, &dialog, [&] {
            const QString path = QFileDialog::getSaveFileName(&dialog, tr("Экспорт своих материалов"), QString(), tr("Библиотека материалов (*.json)"));
            if (path.isEmpty()) return;
            if (auto count = context_.materials().exportUser(std::filesystem::path(path.toStdU16String())); !count) fail(count.error());
        });
        if (dialog.exec() == QDialog::Accepted && list->currentItem()) setMaterial(list->currentItem()->data(Qt::UserRole).toString());
    });
    action(partBar, tr("Исполнения…"), "act_configurations",
           tr("Запомнить нынешние значения параметров как исполнение детали или применить запомненное"), [=, this] {
        QStringList choices;
        const QStringList names = configurationNames();
        for (const QString& name : names) choices << tr("Применить: %1").arg(name);
        choices << tr("Запомнить нынешние размеры как исполнение…");
        bool ok = false;
        const QString choice = QInputDialog::getItem(this, tr("Исполнения"), tr("Что сделать:"), choices, 0, false, &ok);
        if (!ok) return;
        const int index = choices.indexOf(choice);
        if (index < names.size()) {
            applyConfiguration(names.at(index));
            return;
        }
        QString name, designation;
        if (!askText(tr("Исполнение"), tr("Имя исполнения:"), tr("Исполнение %1").arg(names.size() + 1), name)) return;
        if (!askText(tr("Исполнение"), tr("Обозначение исполнения (можно оставить пустым):"), "", designation)) return;
        saveConfiguration(name, designation);
    });
    action(partBar, tr("Зеркало…"), "act_mirror", tr("Добавить к детали её зеркальную копию относительно плоскости"), [=, this] {
        bool ok = false;
        const QStringList names = {tr("YZ — отражение влево-вправо (вдоль X)"), tr("XZ — отражение вперёд-назад (вдоль Y)"),
                                   tr("XY — отражение вверх-вниз (вдоль Z)")};
        const QStringList ids = {"YZ", "XZ", "XY"};
        const QString choice = QInputDialog::getItem(this, tr("Зеркало"), tr("Плоскость отражения:"), names, 0, false, &ok);
        if (!ok) return;
        QString offset;
        if (askText(tr("Зеркало"), tr("Сдвиг плоскости от начала координат, мм (число или формула):"), "0", offset))
            mirror(ids.at(names.indexOf(choice)), offset);
    });
    action(partBar, tr("Откат сюда"), "act_roll_to",
           tr("Показать деталь такой, какой она была после операции, выбранной в дереве. Ничего не выбрано — снять откат"),
           [this] { rollTo(tree_->currentRow()); });
    action(partBar, tr("Вид на чертёж…"), "act_view_to_drawing", tr("Поставить вид модели на открытый чертёж"), [this] {
        bool ok = false;
        const QStringList names = {tr("спереди"), tr("сверху"), tr("слева"), tr("справа"), tr("сзади"), tr("снизу")};
        const QStringList ids = {"front", "top", "left", "right", "back", "bottom"};
        const QString choice = QInputDialog::getItem(this, tr("Вид на чертёж"), tr("Какой вид:"), names, 0, false, &ok);
        if (!ok) return;
        const double x = QInputDialog::getDouble(this, tr("Вид на чертёж"), tr("Середина вида, X на листе, мм:"), 100, -5000, 5000, 1, &ok);
        if (!ok) return;
        const double y = QInputDialog::getDouble(this, tr("Вид на чертёж"), tr("Середина вида, Y на листе, мм:"), 200, -5000, 5000, 1, &ok);
        if (!ok) return;
        const bool hidden = QMessageBox::question(this, tr("Вид на чертёж"), tr("Показывать невидимые линии штриховыми?")) == QMessageBox::Yes;
        addViewToDrawing(ids.at(names.indexOf(choice)), x, y, 1.0, hidden);
    });
    const auto askSection = [=, this](bool onlyCut) {
        const QString title = onlyCut ? tr("Сечение на чертёж") : tr("Разрез на чертёж");
        bool ok = false;
        const QStringList names = {tr("спереди"), tr("сверху"), tr("слева"), tr("справа"), tr("сзади"), tr("снизу")};
        const QStringList ids = {"front", "top", "left", "right", "back", "bottom"};
        const QString choice = QInputDialog::getItem(this, title, tr("Откуда смотреть:"), names, 0, false, &ok);
        if (!ok) return;
        const double x = QInputDialog::getDouble(this, title, tr("Середина изображения, X на листе, мм:"), 100, -5000, 5000, 1, &ok);
        if (!ok) return;
        const double y = QInputDialog::getDouble(this, title, tr("Середина изображения, Y на листе, мм:"), 120, -5000, 5000, 1, &ok);
        if (!ok) return;
        QString at;
        if (!askText(title,
                     tr("Где провести секущую плоскость: координата модели вдоль взгляда, мм.\nОставьте пустым — плоскость пройдёт через середину модели:"), "", at))
            return;
        addSectionToDrawing(ids.at(names.indexOf(choice)), x, y, 1.0, at, onlyCut);
    };
    action(partBar, tr("Разрез на чертёж…"), "act_section_to_drawing",
           tr("Поставить на открытый чертёж разрез модели: видно срез и всё, что за секущей плоскостью"),
           [=] { askSection(false); });
    action(partBar, tr("Сечение на чертёж…"), "act_cut_to_drawing",
           tr("Поставить на открытый чертёж сечение: видно только то, что попало в секущую плоскость"),
           [=] { askSection(true); });
    action(partBar, tr("Сдвинуть разрез…"), "act_move_section", tr("Перенести секущую плоскость готового разреза или сечения"), [=, this] {
        Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
        QStringList labels;
        if (drawing)
            for (const auto* view : drawing->objectsOf<drawing::ModelView>())
                if (view->section) labels << text(view->label);
        if (labels.isEmpty()) {
            fail(tr("на открытом чертеже нет разрезов"));
            return;
        }
        bool ok = false;
        const QString label = QInputDialog::getItem(this, tr("Сдвинуть разрез"), tr("Какой разрез:"), labels, 0, false, &ok);
        if (!ok) return;
        QString at;
        if (askText(tr("Сдвинуть разрез"), tr("Новая координата секущей плоскости, мм:"), "0", at)) moveSection(label, at);
    });
    action(partBar, tr("Обновить виды"), "act_update_views", tr("Перестроить виды на чертеже по текущей модели"),
           [this] { updateDrawingViews(); });

    RibbonGroup* assemblyBar = ribbon->addGroup(assemblyPage, tr("Компоненты и сопряжения"));
    assemblyBar->setCompact(true);
    action(assemblyBar, tr("Добавить компонент…"), "act_add_component", tr("Вставить в сборку деталь или другую сборку проекта"), [this] {
        QStringList names;
        std::vector<Uuid> ids;
        Document* current = currentDocument();
        for (const auto& d : context_.documents().project().documents())
            if (d->kind() == DocumentKind::Part || (d->kind() == DocumentKind::Assembly && d.get() != current)) {
                names << (d->kind() == DocumentKind::Part ? tr("Деталь: ") : tr("Сборка: ")) + text(d->name);
                ids.push_back(d->id());
            }
        if (names.isEmpty()) {
            fail(tr("В проекте нет деталей."));
            return;
        }
        bool ok = false;
        const QString choice = QInputDialog::getItem(this, tr("Компонент"), tr("Что вставить:"), names, 0, false, &ok);
        if (ok) addComponent(ids[static_cast<std::size_t>(names.indexOf(choice))]);
    });
    action(assemblyBar, tr("Крепёж…"), "act_fastener", tr("Болт, гайка или шайба из библиотеки стандартных изделий"), [this] {
        bool ok = false;
        const QStringList kinds = {tr("Болт ГОСТ 7798-70"), tr("Гайка ГОСТ 5915-70"), tr("Шайба ГОСТ 11371-78")};
        const QString kindName = QInputDialog::getItem(this, tr("Крепёж"), tr("Что вставить:"), kinds, 0, false, &ok);
        if (!ok) return;
        const int kind = kinds.indexOf(kindName);
        const auto& library = context_.fasteners();
        const auto fastenerKind = kind == 1 ? libraries::FastenerKind::Nut
                                  : kind == 2 ? libraries::FastenerKind::Washer : libraries::FastenerKind::Bolt;
        QStringList sizes;
        for (const double d : library.diameters(fastenerKind)) sizes << (kind == 2 ? QString() : QStringLiteral("М")) + number(d, 0);
        const QString size = QInputDialog::getItem(this, tr("Крепёж"), tr("Диаметр резьбы:"), sizes, std::min(3, static_cast<int>(sizes.size()) - 1), false, &ok);
        if (!ok) return;
        const double d = library.diameters(fastenerKind).at(static_cast<std::size_t>(sizes.indexOf(size)));
        double length = 0.0;
        if (kind == 0) {
            QStringList lengths;
            const auto values = library.boltLengths(d);
            for (const double l : values) lengths << number(l, 0);
            const QString chosen = QInputDialog::getItem(this, tr("Крепёж"), tr("Длина болта, мм:"), lengths, static_cast<int>(lengths.size()) / 3, false, &ok);
            if (!ok) return;
            length = values.at(static_cast<std::size_t>(lengths.indexOf(chosen)));
        }
        addFastener(kind, d, length);
    });
    action(assemblyBar, tr("Уголок…"), "act_angle", tr("Отрезок стального равнополочного уголка ГОСТ 8509-93"), [this] {
        const auto& profiles = context_.profiles();
        bool ok = false;
        QStringList widths;
        const auto bs = profiles.angleWidths();
        for (const double b : bs) widths << number(b, 0);
        const QString width = QInputDialog::getItem(this, tr("Уголок"), tr("Ширина полки, мм:"), widths, std::min(8, static_cast<int>(widths.size()) - 1), false, &ok);
        if (!ok) return;
        const double b = bs.at(static_cast<std::size_t>(widths.indexOf(width)));
        QStringList thicknesses;
        const auto ts = profiles.angleThicknesses(b);
        for (const double t : ts) thicknesses << number(t, 1);
        const QString thickness = QInputDialog::getItem(this, tr("Уголок"), tr("Толщина полки, мм:"), thicknesses, 0, false, &ok);
        if (!ok) return;
        const double length = QInputDialog::getDouble(this, tr("Уголок"), tr("Длина отрезка, мм:"), 1000.0, 1.0, 100000.0, 1, &ok);
        if (ok) addAngle(b, ts.at(static_cast<std::size_t>(thicknesses.indexOf(thickness))), length);
    });
    action(assemblyBar, tr("Швеллер…"), "act_channel", tr("Отрезок стального швеллера с параллельными гранями полок ГОСТ 8240-97"), [this] {
        bool ok = false;
        QStringList names;
        for (const auto& c : context_.profiles().channels()) names << text(c.name);
        const QString name = QInputDialog::getItem(this, tr("Швеллер"), tr("Номер профиля (П, Э — обычные и экономичные, Л — лёгкие):"), names, 6, false, &ok);
        if (!ok) return;
        const double length = QInputDialog::getDouble(this, tr("Швеллер"), tr("Длина отрезка, мм:"), 1000.0, 1.0, 100000.0, 1, &ok);
        if (ok) addProfile(QStringLiteral("channel"), name, length);
    });
    action(assemblyBar, tr("Двутавр…"), "act_beam", tr("Отрезок стального двутавра ГОСТ Р 57837-2017"), [this] {
        const auto& profiles = context_.profiles();
        bool ok = false;
        QStringList types;
        for (const auto& [mark, title] : profiles.beamTypes()) types << text(mark) + QStringLiteral(" — ") + text(title);
        const QString chosen = QInputDialog::getItem(this, tr("Двутавр"), tr("Тип:"), types, 0, false, &ok);
        if (!ok) return;
        const std::string mark = profiles.beamTypes().at(static_cast<std::size_t>(types.indexOf(chosen))).first;
        QStringList names;
        for (const auto& m : profiles.beams())
            if (m.type == mark) names << text(m.name);
        const QString name = QInputDialog::getItem(this, tr("Двутавр"), tr("Номер профиля:"), names, 0, false, &ok);
        if (!ok) return;
        const double length = QInputDialog::getDouble(this, tr("Двутавр"), tr("Длина отрезка, мм:"), 1000.0, 1.0, 100000.0, 1, &ok);
        if (ok) addProfile(QStringLiteral("beam"), name, length);
    });
    action(assemblyBar, tr("Труба PP…"), "act_pipe", tr("Полипропиленовая труба: расчёт на внутреннее давление и отрезок трубы как деталь"), [this] {
        const auto& pipes = context_.pipes();
        bool ok = false;
        QStringList materials, classes, diameters, series;
        for (const auto& m : pipes.materials()) materials << text(m.name);
        for (const auto& c : pipes.classes()) classes << text(c.name) + QStringLiteral(" — ") + text(c.title);
        for (const double d : pipes.diameters()) diameters << number(d, 0);
        for (const double s : pipes.sdrs()) series << number(s, 1);
        const QString material = QInputDialog::getItem(this, tr("Труба"), tr("Материал:"), materials, 0, false, &ok);
        if (!ok) return;
        const QString chosenClass = QInputDialog::getItem(this, tr("Труба"), tr("Класс эксплуатации:"), classes, 0, false, &ok);
        if (!ok) return;
        const QString diameter = QInputDialog::getItem(this, tr("Труба"), tr("Наружный диаметр, мм:"), diameters, 2, false, &ok);
        if (!ok) return;
        const QString sdr = QInputDialog::getItem(this, tr("Труба"), tr("Серия SDR (диаметр, делённый на толщину стенки):"), series, 1, false, &ok);
        if (!ok) return;
        const double pressure = QInputDialog::getDouble(this, tr("Труба"), tr("Рабочее давление, бар:"), 6.0, 0.0, 100.0, 1, &ok);
        if (!ok) return;
        const double d = pipes.diameters().at(static_cast<std::size_t>(diameters.indexOf(diameter)));
        const double s = pipes.sdrs().at(static_cast<std::size_t>(series.indexOf(sdr)));
        const QString className = text(pipes.classes().at(static_cast<std::size_t>(classes.indexOf(chosenClass))).name);
        const QString report = pipeReport(material, className, d, s, pressure / 10.0);
        if (report.isEmpty()) return;
        if (QMessageBox::question(this, tr("Расчёт трубы"), report + tr("\n\nДобавить отрезок такой трубы в проект?")) != QMessageBox::Yes) return;
        const double length = QInputDialog::getDouble(this, tr("Труба"), tr("Длина отрезка, мм:"), 1000.0, 1.0, 100000.0, 1, &ok);
        if (ok) addPipe(material, d, s, length);
    });
    action(assemblyBar, tr("Совпадение"), "act_mate_coincident", tr("Две плоские грани разных компонентов — вплотную навстречу"),
           [this] { matePicked(assembly::MateKind::Coincident, 0.0, false); });
    action(assemblyBar, tr("Заподлицо"), "act_mate_flush", tr("Две плоские грани — в одной плоскости, смотрят в одну сторону"),
           [this] { matePicked(assembly::MateKind::Coincident, 0.0, true); });
    action(assemblyBar, tr("Соосность"), "act_mate_concentric", tr("Две цилиндрические грани — на одной оси"),
           [this] { matePicked(assembly::MateKind::Concentric, 0.0, false); });
    action(assemblyBar, tr("Расстояние…"), "act_mate_distance", tr("Две плоские грани — параллельно на заданном расстоянии"), [this] {
        bool ok = false;
        const double value = QInputDialog::getDouble(this, tr("Расстояние"), tr("Расстояние, мм:"), 10, -100000, 100000, 2, &ok);
        if (ok) matePicked(assembly::MateKind::Distance, value, false);
    });
    action(assemblyBar, tr("Угол…"), "act_mate_angle", tr("Две плоские грани — под заданным углом"), [this] {
        bool ok = false;
        const double value = QInputDialog::getDouble(this, tr("Угол"), tr("Угол между гранями, градусы (0–180):"), 90, 0, 180, 2, &ok);
        if (ok) matePicked(assembly::MateKind::Angle, value, false);
    });
    action(assemblyBar, tr("Касание"), "act_mate_tangent", tr("Плоская грань одного компонента и цилиндрическая другого — касаются"),
           [this] { matePicked(assembly::MateKind::Tangent, 0.0, false); });
    action(assemblyBar, tr("Не рассекать"), "act_whole_in_section",
           tr("Выбранный в дереве компонент на разрезах показывать целым (валы, болты, шпонки). Повтор снимает пометку"),
           [this] { toggleWholeInSection(tree_->currentRow()); });
    action(assemblyBar, tr("Разнести…"), "act_explode",
           tr("Разнесённый вид: раздвинуть компоненты, чтобы было видно, как изделие собирается. 0 — собрать обратно"), [this] {
        bool ok = false;
        const double distance = QInputDialog::getDouble(this, tr("Разнесённый вид"), tr("На сколько раздвинуть компоненты, мм (0 — собрать):"),
                                                        exploded_ ? 0 : 40, 0, 100000, 1, &ok);
        if (ok) explode(distance);
    });
    action(assemblyBar, tr("Степени свободы"), "act_freedom", tr("Сколько движений осталось у каждого компонента при нынешних сопряжениях"),
           [this] { QMessageBox::information(this, tr("Степени свободы"), freedomText()); });
    action(assemblyBar, tr("Пересечения"), "act_interference", tr("Проверить, не врезаются ли компоненты друг в друга"),
           [this] { QMessageBox::information(this, tr("Проверка пересечений"), interferenceReport()); });
    action(assemblyBar, tr("Зазор"), "act_clearance", tr("Щёлкните по грани на двух компонентах — программа измерит зазор между ними"),
           [this] { QMessageBox::information(this, tr("Зазор"), clearanceText()); });
    action(assemblyBar, tr("Состав изделия"), "act_bom", tr("Перечень деталей сборки с количеством и массой"),
           [this] { QMessageBox::information(this, tr("Состав изделия"), billOfMaterialsText()); });

    action(assemblyBar, tr("Состав в CSV…"), "act_bom_csv", tr("Сохранить состав изделия таблицей (открывается в Excel)"), [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Состав изделия"), QString(), tr("Таблица CSV (*.csv)"));
        if (!path.isEmpty()) exportBillOfMaterials(std::filesystem::path(path.toStdU16String()));
    });

    RibbonGroup* viewBar = ribbon->addGroup(modelPage, tr("Вид"));
    action(viewBar, tr("Спереди"), "act_view_front", tr("Вид спереди"), [this] { view_->fit(-90, 0); });
    action(viewBar, tr("Сверху"), "act_view_top", tr("Вид сверху"), [this] { view_->fit(-90, 90); });
    action(viewBar, tr("Слева"), "act_view_left", tr("Вид слева"), [this] { view_->fit(180, 0); });
    action(viewBar, tr("Изометрия"), "act_view_iso", tr("Вид под углом"), [this] { view_->fit(-60, 30); });
}

bool ModelWindow::fail(const core::Error& error) { return fail(text(error.message)); }

bool ModelWindow::fail(const QString& message) {
    messages_->setStyleSheet("color: #b00020;");
    messages_->setText(tr("Не выполнено: ") + message);
    return false;
}

void ModelWindow::done(const QString& message) {
    messages_->setStyleSheet(QString());
    messages_->setText(message);
}

Document* ModelWindow::currentDocument() const {
    const auto id = Uuid::parse(documents_->currentData().toString().toStdString());
    return id ? context_.documents().findDocument(id.value()) : nullptr;
}

void ModelWindow::selectDocument(const Uuid& id) {
    const int index = documents_->findData(text(id.toString()));
    if (index >= 0) documents_->setCurrentIndex(index);
}

void ModelWindow::refresh() {
    updating_ = true;
    const QVariant current = documents_->currentData();
    documents_->clear();
    for (const auto& d : context_.documents().project().documents()) {
        if (d->kind() == DocumentKind::Part)
            documents_->addItem(tr("Деталь: ") + text(d->name), text(d->id().toString()));
        else if (d->kind() == DocumentKind::Assembly)
            documents_->addItem(tr("Сборка: ") + text(d->name), text(d->id().toString()));
    }
    const int index = documents_->findData(current);
    documents_->setCurrentIndex(index >= 0 ? index : (documents_->count() > 0 ? 0 : -1));
    updating_ = false;
    if (index < 0) {
        view_->clearPicked();
        view_->setBodies({});
    }
    rebuild();
}

void ModelWindow::rebuild() {
    updating_ = true;
    tree_->clear();
    parameters_->setRowCount(0);
    info_->clear();
    componentOrder_.clear();
    std::vector<geometry::Mesh> meshes;
    Document* document = currentDocument();

    if (document && document->kind() == DocumentKind::Part) {
        auto report = context_.modeling().recompute(*document);
        if (!report) fail(report.error());
        const auto statuses = context_.modeling().status(*document);
        std::size_t row = 0;
        for (const auto* feature : context_.modeling().features(*document)) {
            modeling::FeatureStatus status;
            if (row < statuses.size()) status = statuses[row];
            ++row;
            const bool failed = status.state == modeling::FeatureState::Failed;
            auto* item = new QListWidgetItem(tree_);
            QString patternNote;
            if (feature->pattern.kind == modeling::Pattern::Kind::Linear) patternNote = tr(", в ряд × ") + text(feature->pattern.count);
            if (feature->pattern.kind == modeling::Pattern::Kind::Circular) patternNote = tr(", по кругу × ") + text(feature->pattern.count);
            if (feature->kind == modeling::FeatureKind::Mirror) patternNote = tr(" мм от плоскости ") + text(feature->axis);
            item->setText((failed ? QStringLiteral("⚠ ") : QString()) + text(feature->name) + " = " + text(feature->value) + patternNote +
                          (failed ? tr(" — сбой: ") + text(status.diagnostic) : QString()));
            item->setToolTip(failed ? text(status.diagnostic) : tr("Двойной щелчок — изменить размер"));
            item->setData(Qt::UserRole, text(feature->id().toString()));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(feature->suppressed ? Qt::Unchecked : Qt::Checked);
            if (failed) item->setForeground(QColor(0xb0, 0x00, 0x20));
            if (status.state == modeling::FeatureState::Suppressed && status.diagnostic == "после отката") {
                item->setText(item->text() + tr(" — ждёт (откат)"));
                item->setForeground(QColor(0x9a, 0xa2, 0xad));
            }
        }
        if (auto values = context_.modeling().parameterValues(*document)) {
            for (const auto* parameter : document->objectsOf<modeling::Parameter>()) {
                const int r = parameters_->rowCount();
                parameters_->insertRow(r);
                auto* name = new QTableWidgetItem(text(parameter->name));
                name->setFlags(name->flags() & ~Qt::ItemIsEditable);
                auto* value = new QTableWidgetItem(number(values.value().at(parameter->name), 3));
                value->setFlags(value->flags() & ~Qt::ItemIsEditable);
                parameters_->setItem(r, 0, name);
                parameters_->setItem(r, 1, new QTableWidgetItem(text(parameter->expression)));
                parameters_->setItem(r, 2, value);
            }
        }
        if (const auto body = context_.modeling().body(*document)) {
            if (auto mesh = context_.kernel().tessellate(body, 0.1)) meshes.push_back(mesh.take());
            if (auto mass = context_.modeling().mass(*document)) {
                const auto& g = mass.value().geometry;
                info_->setText(tr("Объём %1 см³ · масса %2 г · площадь %3 см²\nЦентр масс: %4; %5; %6 мм")
                                   .arg(number(g.volume / 1000.0), number(mass.value().massGrams, 1),
                                        number(g.surfaceArea / 100.0), number(g.centerOfMass.x), number(g.centerOfMass.y),
                                        number(g.centerOfMass.z)));
            }
        } else {
            info_->setText(tr("Тела пока нет. Нарисуйте на чертеже замкнутый контур, выберите его и нажмите «Выдавить контур»."));
        }
    } else if (document && document->kind() == DocumentKind::Assembly) {
        for (const auto* component : document->objectsOf<assembly::Component>()) {
            auto* item = new QListWidgetItem(tree_);
            item->setText(text(component->name) + (component->fixed ? tr(" — закреплён") : QString()) +
                          (component->wholeInSection ? tr(" — не рассекать") : QString()));
            item->setData(Qt::UserRole, text(component->id().toString()));
        }
        for (const auto* mate : document->objectsOf<assembly::Mate>()) {
            auto* item = new QListWidgetItem(tree_);
            item->setText(tr("Сопряжение: ") + text(mate->name));
            item->setData(Qt::UserRole, text(mate->id().toString()));
        }
        if (auto placed = context_.assembly().placedBodies(*document, exploded_)) {
            for (const auto& body : placed.value())
                if (auto mesh = context_.kernel().tessellate(body.shape, 0.1)) {
                    meshes.push_back(mesh.take());
                    componentOrder_.push_back(body.componentId);
                }
        } else {
            fail(placed.error());
        }
        info_->setText(tr("Компонентов: %1, сопряжений: %2")
                           .arg(document->objectsOf<assembly::Component>().size())
                           .arg(document->objectsOf<assembly::Mate>().size()));
    } else {
        info_->setText(tr("В проекте нет ни детали, ни сборки. Нажмите «Новая деталь»."));
    }
    view_->setBodies(std::move(meshes));
    updating_ = false;
    onPickedChanged();
}

void ModelWindow::onPickedChanged() {
    QStringList parts;
    Document* document = currentDocument();
    for (const auto& face : view_->pickedFaces()) {
        QString line = text(face.name);
        if (document && document->kind() == DocumentKind::Part) {
            if (const auto body = context_.modeling().body(*document))
                if (auto info = context_.kernel().faceInfo(body, face.name)) {
                    if (info.value().kind == geometry::FaceInfo::Kind::Plane) line += tr(" (плоскость)");
                    if (info.value().kind == geometry::FaceInfo::Kind::Cylinder)
                        line += tr(" (цилиндр ⌀%1)").arg(number(info.value().radius * 2));
                }
        }
        parts << line;
    }
    pickedLabel_->setText(parts.isEmpty() ? tr("Грани не выбраны.") : tr("Выбраны грани: ") + parts.join("; "));
}

// ---- Действия ----

bool ModelWindow::newPart(const QString& name) {
    auto created = context_.documents().createDocument(DocumentKind::Part, name.toStdString());
    if (!created) return fail(created.error());
    refresh();
    selectDocument(created.value()->id());
    done(tr("Создана деталь «%1».").arg(name));
    emit projectChanged();
    return true;
}

bool ModelWindow::newAssembly(const QString& name) {
    auto created = context_.documents().createDocument(DocumentKind::Assembly, name.toStdString());
    if (!created) return fail(created.error());
    refresh();
    selectDocument(created.value()->id());
    done(tr("Создана сборка «%1».").arg(name));
    emit projectChanged();
    return true;
}

bool ModelWindow::addParameter(const QString& name, const QString& expression) {
    Document* document = currentDocument();
    if (!document) return fail(tr("нет открытой детали"));
    auto added = context_.modeling().addParameter(*document, name.trimmed().toStdString(), expression.toStdString());
    if (!added) return fail(added.error());
    rebuild();
    done(tr("Параметр «%1» добавлен.").arg(name));
    emit projectChanged();
    return true;
}

bool ModelWindow::setParameter(const QString& name, const QString& expression) {
    Document* document = currentDocument();
    if (!document) return fail(tr("нет открытой детали"));
    auto changed = context_.modeling().setParameter(*document, name.toStdString(), expression.toStdString());
    if (!changed) {
        rebuild();  // вернуть в таблицу прежнюю формулу
        return fail(changed.error());
    }
    // Ограничения на чертеже с формулами от этого параметра и эскизы, снятые с контуров, пересчитываются.
    if (auto synced = context_.syncSketches(*document); !synced) {
        document->undo();
        (void)context_.syncSketches(*document);
        rebuild();
        return fail(synced.error());
    }
    emit drawingChanged();
    rebuild();
    done(tr("Параметр «%1» изменён, модель перестроена.").arg(name));
    emit projectChanged();
    return true;
}

void ModelWindow::onParameterEdited(int row, int column) {
    if (updating_ || column != 1) return;
    setParameter(parameters_->item(row, 0)->text(), parameters_->item(row, 1)->text());
}

bool ModelWindow::extrudeContour(const QString& distance, const QString& planeOffset, bool cut) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите или создайте деталь"));
    Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
    if (!drawing) return fail(tr("нет открытого чертежа с контуром"));
    const std::vector<Uuid> selected = drawingSelection_ ? drawingSelection_() : std::vector<Uuid>();
    auto shapes = app::AppContext::contourFromDrawing(*drawing, selected);
    if (!shapes) return fail(shapes.error());
    // Эскиз помнит, с какого контура снят: изменится контур на чертеже — обновится и тело.
    auto sketch = context_.modeling().addSketch(*document, {"", "XY", planeOffset.toStdString(), "", shapes.value(), drawing->id(), selected});
    if (!sketch) return fail(sketch.error());
    auto feature = context_.modeling().addExtrude(*document, {sketch.value(), distance.toStdString(), cut, false});
    if (!feature) {
        document->undo();  // убрать эскиз, оставшийся без операции
        return fail(feature.error());
    }
    rebuild();
    done(cut ? tr("Вырез добавлен.") : tr("Выдавливание добавлено."));
    emit projectChanged();
    return true;
}

bool ModelWindow::sweepContour(const QString& path, const QString& bendRadius, bool cut) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите или создайте деталь"));
    Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
    if (!drawing) return fail(tr("нет открытого чертежа с контуром"));
    std::vector<std::array<std::string, 3>> points;
    for (const QString& part : path.split(';', Qt::SkipEmptyParts)) {
        const QStringList numbers = part.simplified().split(' ', Qt::SkipEmptyParts);
        if (numbers.size() != 3)
            return fail(tr("каждая точка траектории — три числа через пробел (x y z), а введено «%1»").arg(part.trimmed()));
        points.push_back({numbers[0].toStdString(), numbers[1].toStdString(), numbers[2].toStdString()});
    }
    if (points.empty()) return fail(tr("задайте хотя бы одну точку траектории, например «0 0 50»"));
    auto shapes = app::AppContext::contourFromDrawing(*drawing, drawingSelection_ ? drawingSelection_() : std::vector<Uuid>());
    if (!shapes) return fail(shapes.error());
    auto sketch = context_.modeling().addSketch(*document, {"", "XY", "0", "", shapes.value()});
    if (!sketch) return fail(sketch.error());
    auto feature = context_.modeling().addSweep(*document, {sketch.value(), points, bendRadius.trimmed().toStdString(), cut});
    if (!feature) {
        document->undo();  // убрать эскиз, оставшийся без операции
        return fail(feature.error());
    }
    rebuild();
    done(tr("Протяжка добавлена."));
    emit projectChanged();
    return true;
}

bool ModelWindow::blendPicked(const QString& size, bool chamfer) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    const auto& picked = view_->pickedFaces();
    if (picked.size() != 2) return fail(tr("щёлкните в окне 3D две грани, между которыми лежит ребро"));
    const geometry::EdgeRef edge{picked[0].name, picked[1].name};
    if (!context_.kernel().hasEdge(context_.modeling().body(*document), edge))
        return fail(tr("выбранные грани не соседние: между ними нет ребра"));
    auto feature = context_.modeling().addBlend(*document, {{edge}, size.toStdString(), chamfer});
    if (!feature) return fail(feature.error());
    view_->clearPicked();
    rebuild();
    done(chamfer ? tr("Фаска добавлена.") : tr("Скругление добавлено."));
    emit projectChanged();
    return true;
}

std::optional<geometry::Point2> ModelWindow::pickedPointOnFace() const {
    Document* document = currentDocument();
    const auto& picked = view_->pickedFaces();
    if (!document || document->kind() != DocumentKind::Part || picked.size() != 1) return std::nullopt;
    const auto body = context_.modeling().body(*document);
    if (!body) return std::nullopt;
    auto info = context_.kernel().faceInfo(body, picked[0].name);
    auto plane = context_.modeling().facePlane(*document, picked[0].name);
    if (!info || !plane || info.value().kind != geometry::FaceInfo::Kind::Plane) return std::nullopt;
    const auto point = view_->pointOnPlane(picked[0].pixel, info.value().point, info.value().direction);
    if (!point) return std::nullopt;
    // Координаты точки в осях эскиза на этой грани.
    const auto& p = plane.value();
    const geometry::Vector3 yAxis = geometry::crossProduct(p.normal, p.xDirection);
    const geometry::Vector3 d{point->x - p.origin.x, point->y - p.origin.y, point->z - p.origin.z};
    return geometry::Point2{geometry::dotProduct(d, p.xDirection), geometry::dotProduct(d, yAxis)};
}

bool ModelWindow::addShapeOnPickedFace(bool circle, const QString& x, const QString& y, const QString& size1,
                                       const QString& size2, const QString& depth, bool cut) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    const auto& picked = view_->pickedFaces();
    if (picked.size() != 1) return fail(tr("щёлкните по одной плоской грани — в том месте, где нужен элемент"));
    modeling::SketchShape shape;
    const auto text = [](const QString& value) { return value.trimmed().toStdString(); };
    if (circle) {
        shape.kind = modeling::SketchShape::Kind::Circle;
        shape.values = {text(x), text(y), "(" + text(size1) + ") / 2"};
    } else {
        shape.kind = modeling::SketchShape::Kind::Rectangle;
        shape.values = {"(" + text(x) + ") - (" + text(size1) + ") / 2", "(" + text(y) + ") - (" + text(size2) + ") / 2",
                        text(size1), text(size2)};
    }
    auto sketch = context_.modeling().addSketch(*document, {"", "", "0", picked[0].name, {shape}});
    if (!sketch) return fail(sketch.error());
    // Вырез идёт внутрь тела (против нормали грани), выступ — наружу.
    auto feature = context_.modeling().addExtrude(*document, {sketch.value(), text(depth), cut, cut});
    if (!feature) {
        document->undo();  // убрать эскиз, оставшийся без операции
        return fail(feature.error());
    }
    view_->clearPicked();
    rebuild();
    emit projectChanged();
    for (const auto& status : context_.modeling().status(*document))
        if (status.featureId == feature.value() && status.state == modeling::FeatureState::Failed)
            return fail(tr("операция добавлена, но не построилась: ") + QString::fromStdString(status.diagnostic));
    done(cut ? tr("Вырез на грани добавлен.") : tr("Выступ на грани добавлен."));
    return true;
}

bool ModelWindow::setPattern(int row, int kind, const QString& count, const QString& a, const QString& b,
                             const QString& angle) {
    Document* document = currentDocument();
    if (!document || row < 0 || row >= tree_->count()) return fail(tr("операция не выбрана"));
    modeling::Pattern pattern;
    if (kind == 1) {
        pattern.kind = modeling::Pattern::Kind::Linear;
        pattern.stepX = a.trimmed().toStdString();
        pattern.stepY = b.trimmed().toStdString();
    } else if (kind == 2) {
        pattern.kind = modeling::Pattern::Kind::Circular;
        pattern.centerX = a.trimmed().toStdString();
        pattern.centerY = b.trimmed().toStdString();
        pattern.angle = angle.trimmed().toStdString();
    }
    pattern.count = kind == 0 ? std::string("1") : count.trimmed().toStdString();
    auto changed = context_.modeling().setPattern(*document, idOf(tree_->item(row)), pattern);
    if (!changed) return fail(changed.error());
    rebuild();
    emit projectChanged();
    const auto statuses = context_.modeling().status(*document);
    if (static_cast<std::size_t>(row) < statuses.size() && statuses[static_cast<std::size_t>(row)].state == modeling::FeatureState::Failed)
        return fail(QString::fromStdString(statuses[static_cast<std::size_t>(row)].diagnostic));
    done(kind == 0 ? tr("Массив убран.") : tr("Массив задан, модель перестроена."));
    return true;
}

QStringList ModelWindow::sketchValueLabels(int row) const {
    QStringList out;
    Document* document = currentDocument();
    if (!document || row < 0 || row >= tree_->count()) return out;
    const auto* feature = document->findAs<modeling::Feature>(idOf(tree_->item(row)));
    const auto* sketch = feature ? document->findAs<modeling::Sketch>(feature->sketchId) : nullptr;
    if (!sketch) return out;
    int number = 1;
    for (const auto& shape : sketch->shapes) {
        QStringList names;
        QString kind;
        if (shape.kind == modeling::SketchShape::Kind::Rectangle) {
            kind = tr("прямоугольник");
            names = QStringList{tr("X левого нижнего угла"), tr("Y левого нижнего угла"), tr("ширина"), tr("высота")};
        } else if (shape.kind == modeling::SketchShape::Kind::Circle) {
            kind = tr("окружность");
            names = QStringList{tr("X центра"), tr("Y центра"), tr("радиус")};
        } else {
            kind = tr("многоугольник");
            for (std::size_t i = 0; i < shape.values.size(); ++i)
                names << (i % 2 == 0 ? tr("X точки %1") : tr("Y точки %1")).arg(i / 2 + 1);
        }
        for (const auto& name : names) out << tr("Фигура %1 (%2%3): %4").arg(number).arg(kind, shape.hole ? tr(", отверстие") : QString(), name);
        ++number;
    }
    return out;
}

QStringList ModelWindow::sketchValues(int row) const {
    QStringList out;
    Document* document = currentDocument();
    if (!document || row < 0 || row >= tree_->count()) return out;
    const auto* feature = document->findAs<modeling::Feature>(idOf(tree_->item(row)));
    const auto* sketch = feature ? document->findAs<modeling::Sketch>(feature->sketchId) : nullptr;
    if (!sketch) return out;
    for (const auto& shape : sketch->shapes)
        for (const auto& value : shape.values) out << QString::fromStdString(value);
    return out;
}

bool ModelWindow::setSketchValue(int row, int shape, int value, const QString& expression) {
    Document* document = currentDocument();
    if (!document || row < 0 || row >= tree_->count()) return fail(tr("операция не выбрана"));
    const auto* feature = document->findAs<modeling::Feature>(idOf(tree_->item(row)));
    const auto* sketch = feature ? document->findAs<modeling::Sketch>(feature->sketchId) : nullptr;
    if (!sketch) return fail(tr("у этой операции нет эскиза"));
    auto shapes = sketch->shapes;
    if (shape < 0 || static_cast<std::size_t>(shape) >= shapes.size() || value < 0 ||
        static_cast<std::size_t>(value) >= shapes[static_cast<std::size_t>(shape)].values.size())
        return fail(tr("нет такого размера эскиза"));
    shapes[static_cast<std::size_t>(shape)].values[static_cast<std::size_t>(value)] = expression.trimmed().toStdString();
    auto changed = context_.modeling().updateSketch(*document, sketch->id(), shapes);
    if (!changed) return fail(changed.error());
    rebuild();
    done(tr("Размер эскиза изменён, модель перестроена."));
    emit projectChanged();
    return true;
}

bool ModelWindow::setFeatureValue(int row, const QString& expression) {
    Document* document = currentDocument();
    if (!document || row < 0 || row >= tree_->count()) return fail(tr("операция не выбрана"));
    auto changed = context_.modeling().setFeatureValue(*document, idOf(tree_->item(row)), expression.toStdString());
    if (!changed) return fail(changed.error());
    rebuild();
    done(tr("Размер изменён, модель перестроена."));
    emit projectChanged();
    return true;
}

void ModelWindow::onTreeItemChanged(QListWidgetItem* item) {
    if (updating_) return;
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return;
    auto changed = context_.modeling().suppressFeature(*document, idOf(item), item->checkState() != Qt::Checked);
    if (!changed) fail(changed.error());
    rebuild();
    emit projectChanged();
}

bool ModelWindow::rollTo(int row) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    Uuid id;
    if (row >= 0 && row < tree_->count()) id = idOf(tree_->item(row));
    // Повторный откат на ту же операцию снимает его.
    if (const auto properties = document->objectsOf<modeling::PartProperties>(); !properties.empty() && properties.front()->rollbackAfter == id)
        id = Uuid();
    auto changed = context_.modeling().rollTo(*document, id);
    if (!changed) return fail(changed.error());
    rebuild();
    done(id.isNil() ? tr("Откат снят: деталь построена полностью.") : tr("Откат: деталь показана после выбранной операции."));
    emit projectChanged();
    return true;
}

bool ModelWindow::explode(double distance) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return fail(tr("сначала выберите сборку"));
    auto moved = context_.assembly().autoExplode(*document, distance);
    if (!moved) return fail(moved.error());
    exploded_ = distance > 0.0;
    view_->clearPicked();
    rebuild();
    view_->fit(view_->camera().yawDegrees, view_->camera().pitchDegrees);
    done(exploded_ ? tr("Сборка показана разнесённой. Сопряжения и собранный вид не изменились.") : tr("Сборка собрана обратно."));
    emit projectChanged();
    return true;
}

QStringList ModelWindow::configurationNames() {
    QStringList names;
    Document* document = currentDocument();
    if (document && document->kind() == DocumentKind::Part)
        for (const auto& name : context_.modeling().configurations(*document)) names << text(name);
    return names;
}

bool ModelWindow::saveConfiguration(const QString& name, const QString& designation) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    auto saved = context_.modeling().saveConfiguration(*document, name.trimmed().toStdString(), designation.trimmed().toStdString());
    if (!saved) return fail(saved.error());
    done(tr("Исполнение «%1» запомнено.").arg(name.trimmed()));
    emit projectChanged();
    return true;
}

bool ModelWindow::applyConfiguration(const QString& name) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    auto applied = context_.modeling().applyConfiguration(*document, name.toStdString());
    if (!applied) return fail(applied.error());
    if (auto synced = context_.syncSketches(*document); !synced) fail(synced.error());
    emit drawingChanged();
    rebuild();
    done(tr("Применено исполнение «%1».").arg(name));
    emit projectChanged();
    return true;
}

QStringList ModelWindow::materialTitles(const QString& filter) {
    QStringList titles;
    for (const auto& material : context_.materials().find(filter.toStdString())) titles << text(material.title());
    return titles;
}

bool ModelWindow::setMaterial(const QString& title) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    auto changed = context_.setPartMaterial(*document, title.toStdString());
    if (!changed) return fail(changed.error());
    rebuild();
    done(tr("Материал детали: %1.").arg(title));
    emit projectChanged();
    return true;
}

bool ModelWindow::addUserMaterial(const QString& name, const QString& standard, double density, const QString& note) {
    libraries::Material material;
    material.name = name.trimmed().toStdString();
    material.standard = standard.trimmed().toStdString();
    material.density = density;
    material.note = note.trimmed().toStdString();
    auto added = context_.materials().addUser(material);
    if (!added) return fail(added.error());
    done(tr("Материал «%1» добавлен в свои материалы.").arg(text(material.title())));
    return true;
}

bool ModelWindow::mirror(const QString& plane, const QString& offset) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Part) return fail(tr("сначала выберите деталь"));
    auto added = context_.modeling().addMirror(*document, {plane.toStdString(), offset.trimmed().toStdString()});
    if (!added) return fail(added.error());
    rebuild();
    done(tr("Зеркальная копия добавлена."));
    emit projectChanged();
    return true;
}

bool ModelWindow::removeSelected() {
    Document* document = currentDocument();
    QListWidgetItem* item = tree_->currentItem();
    if (!document || !item) return fail(tr("выберите строку в дереве"));
    const Uuid id = idOf(item);
    core::Result<void> removed = core::makeError(core::ErrorCode::NotFound, "строка не найдена");
    if (document->findAs<modeling::Feature>(id))
        removed = context_.modeling().removeFeature(*document, id);
    else if (document->findAs<assembly::Component>(id))
        removed = context_.assembly().removeComponent(*document, id);
    else if (document->findAs<assembly::Mate>(id))
        removed = context_.assembly().removeMate(*document, id);
    if (!removed) return fail(removed.error());
    view_->clearPicked();
    rebuild();
    done(tr("Удалено."));
    emit projectChanged();
    return true;
}

bool ModelWindow::undo() {
    Document* document = currentDocument();
    if (!document || !document->undo()) return fail(tr("отменять нечего"));
    rebuild();
    done(tr("Действие отменено."));
    emit projectChanged();
    return true;
}

bool ModelWindow::redo() {
    Document* document = currentDocument();
    if (!document || !document->redo()) return fail(tr("повторять нечего"));
    rebuild();
    done(tr("Действие повторено."));
    emit projectChanged();
    return true;
}

bool ModelWindow::addViewToDrawing(const QString& viewName, double x, double y, double scale, bool showHidden) {
    Document* document = currentDocument();
    Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
    if (!document) return fail(tr("нет модели"));
    if (!drawing) return fail(tr("нет открытого чертежа"));
    const bool exploded = exploded_ && document->kind() == DocumentKind::Assembly;
    auto added = context_.addModelView(*drawing, *document, viewName.toStdString(), {x, y}, scale, showHidden, exploded);
    if (!added) return fail(added.error());
    done(tr("Вид поставлен на чертёж «%1».").arg(text(drawing->name)));
    emit drawingChanged();
    return true;
}

bool ModelWindow::toggleWholeInSection(int row) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return fail(tr("сначала выберите сборку"));
    const auto* component = row >= 0 && row < tree_->count() ? document->findAs<assembly::Component>(idOf(tree_->item(row))) : nullptr;
    if (!component) return fail(tr("выберите компонент в дереве"));
    const bool whole = !component->wholeInSection;
    auto changed = context_.assembly().setWholeInSection(*document, component->id(), whole);
    if (!changed) return fail(changed.error());
    rebuild();
    done(whole ? tr("Компонент на разрезах будет показан целым. Нажмите «Обновить виды».")
               : tr("Компонент на разрезах будет рассекаться. Нажмите «Обновить виды»."));
    emit projectChanged();
    return true;
}

bool ModelWindow::addSectionToDrawing(const QString& viewName, double x, double y, double scale, const QString& at,
                                      bool onlyCut) {
    Document* document = currentDocument();
    Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
    if (!document) return fail(tr("нет модели"));
    if (!drawing) return fail(tr("нет открытого чертежа"));
    std::optional<double> position;
    if (!at.trimmed().isEmpty()) {
        bool ok = false;
        position = QString(at).replace(',', '.').trimmed().toDouble(&ok);
        if (!ok) return fail(tr("положение секущей плоскости должно быть числом, а введено «%1»").arg(at));
    }
    auto added = context_.addSectionView(*drawing, *document, viewName.toStdString(), {x, y}, scale, position, onlyCut);
    if (!added) return fail(added.error());
    const auto* view = drawing->findAs<drawing::ModelView>(added.value());
    done((onlyCut ? tr("Сечение %1 поставлено на чертёж «%2».") : tr("Разрез %1 поставлен на чертёж «%2»."))
             .arg(text(view ? view->label : std::string()), text(drawing->name)));
    emit drawingChanged();
    return true;
}

bool ModelWindow::moveSection(const QString& label, const QString& at) {
    Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
    if (!drawing) return fail(tr("нет открытого чертежа"));
    bool ok = false;
    const double position = QString(at).replace(',', '.').trimmed().toDouble(&ok);
    if (!ok) return fail(tr("положение секущей плоскости должно быть числом, а введено «%1»").arg(at));
    for (const auto* view : drawing->objectsOf<drawing::ModelView>()) {
        if (!view->section || text(view->label) != label) continue;
        auto moved = context_.moveSection(*drawing, view->id(), position);
        if (!moved) return fail(moved.error());
        done(tr("Секущая плоскость %1 перенесена.").arg(label));
        emit drawingChanged();
        return true;
    }
    return fail(tr("разрез %1 на открытом чертеже не найден").arg(label));
}

QString ModelWindow::freedomText() {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return tr("Степени свободы считаются для сборки.");
    auto found = context_.assembly().degreesOfFreedom(*document);
    if (!found) return tr("Не выполнено: ") + text(found.error().message);
    QStringList lines;
    for (const auto& item : found.value()) {
        const auto* component = document->findAs<assembly::Component>(item.componentId);
        const QString name = text(component ? component->name : std::string("?"));
        if (item.fixed)
            lines << tr("%1: закреплён").arg(name);
        else if (item.free == 0)
            lines << tr("%1: положение определено полностью").arg(name);
        else
            lines << tr("%1: свободных движений — %2 из 6").arg(name).arg(item.free);
    }
    return lines.isEmpty() ? tr("В сборке нет компонентов.") : lines.join("\n");
}

bool ModelWindow::updateDrawingViews() {
    Document* drawing = currentDrawing_ ? currentDrawing_() : nullptr;
    if (!drawing) return fail(tr("нет открытого чертежа"));
    auto updated = context_.updateModelViews(*drawing);
    if (!updated) return fail(updated.error());
    const auto& dims = context_.lastViewUpdate();
    QString note = tr("Обновлено видов на чертеже: %1; размеров переехало за моделью: %2.").arg(updated.value()).arg(dims.movedDimensions);
    if (dims.lostDimensions > 0) {
        fail(note + tr(" Потеряли привязку: %1 — они остались на месте, поправьте их вручную.").arg(dims.lostDimensions));
        emit drawingChanged();
        return true;
    }
    done(note);
    emit drawingChanged();
    return true;
}

bool ModelWindow::exportStep(const std::filesystem::path& path) {
    Document* document = currentDocument();
    if (!document) return fail(tr("нет модели"));
    auto bodies = context_.bodiesOf(*document);
    if (!bodies) return fail(bodies.error());
    if (auto saved = context_.kernel().exportStep(bodies.value(), path); !saved) return fail(saved.error());
    done(tr("Сохранено в STEP."));
    return true;
}

bool ModelWindow::exportStl(const std::filesystem::path& path) {
    Document* document = currentDocument();
    if (!document) return fail(tr("нет модели"));
    auto bodies = context_.bodiesOf(*document);
    if (!bodies) return fail(bodies.error());
    auto whole = context_.kernel().combine(bodies.value());
    if (!whole) return fail(whole.error());
    if (auto saved = context_.kernel().exportStl(whole.value(), path, 0.05); !saved) return fail(saved.error());
    done(tr("Сохранено в STL."));
    return true;
}

bool ModelWindow::addFastener(int kind, double d, double length) {
    libraries::FastenerRequest request;
    request.kind = kind == 1 ? libraries::FastenerKind::Nut : kind == 2 ? libraries::FastenerKind::Washer : libraries::FastenerKind::Bolt;
    request.d = d;
    request.length = length;
    Document* current = currentDocument();
    auto part = context_.standardPart(request);
    if (!part) return fail(part.error());
    const QString name = text(part.value()->name);
    if (current && current->kind() == DocumentKind::Assembly) {
        refresh();
        selectDocument(current->id());
        if (!addComponent(part.value()->id())) return false;
        done(tr("%1 — вставлено в сборку.").arg(name));
        return true;
    }
    refresh();
    selectDocument(part.value()->id());
    done(tr("%1 — деталь добавлена в проект.").arg(name));
    emit projectChanged();
    return true;
}

bool ModelWindow::addAngle(double b, double t, double length) {
    Document* current = currentDocument();
    auto part = context_.anglePart(b, t, length);
    if (!part) return fail(part.error());
    const QString name = text(part.value()->name);
    if (current && current->kind() == DocumentKind::Assembly) {
        refresh();
        selectDocument(current->id());
        if (!addComponent(part.value()->id())) return false;
        done(tr("%1 — вставлено в сборку.").arg(name));
        return true;
    }
    refresh();
    selectDocument(part.value()->id());
    done(tr("%1 — деталь добавлена в проект.").arg(name));
    emit projectChanged();
    return true;
}

bool ModelWindow::addProfile(const QString& kind, const QString& name, double length) {
    Document* current = currentDocument();
    auto part = context_.profilePart(kind.toStdString(), name.toStdString(), length);
    if (!part) return fail(part.error());
    const QString title = text(part.value()->name);
    if (current && current->kind() == DocumentKind::Assembly) {
        refresh();
        selectDocument(current->id());
        if (!addComponent(part.value()->id())) return false;
        done(tr("%1 — вставлено в сборку.").arg(title));
        return true;
    }
    refresh();
    selectDocument(part.value()->id());
    done(tr("%1 — деталь добавлена в проект.").arg(title));
    emit projectChanged();
    return true;
}

QString ModelWindow::pipeReport(const QString& material, const QString& serviceClass, double diameter, double sdr, double pressureMpa) {
    const auto& pipes = context_.pipes();
    auto wall = pipes.wall(diameter, sdr);
    if (!wall) {
        fail(wall.error());
        return {};
    }
    auto report = pipes.check(material.toStdString(), serviceClass.toStdString(), diameter, wall.value(), pressureMpa);
    if (!report) {
        fail(report.error());
        return {};
    }
    QStringList lines;
    for (const auto& line : report.value().lines) lines << text(line);
    return lines.join(QLatin1Char('\n'));
}

bool ModelWindow::addPipe(const QString& material, double diameter, double sdr, double length) {
    auto wall = context_.pipes().wall(diameter, sdr);
    if (!wall) return fail(wall.error());
    Document* current = currentDocument();
    auto part = context_.pipePart(material.toStdString(), diameter, wall.value(), length);
    if (!part) return fail(part.error());
    const QString name = text(part.value()->name);
    if (current && current->kind() == DocumentKind::Assembly) {
        refresh();
        selectDocument(current->id());
        if (!addComponent(part.value()->id())) return false;
        done(tr("%1 — вставлено в сборку.").arg(name));
        return true;
    }
    refresh();
    selectDocument(part.value()->id());
    done(tr("%1 — деталь добавлена в проект.").arg(name));
    emit projectChanged();
    return true;
}

bool ModelWindow::addComponent(const Uuid& partDocumentId) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return fail(tr("сначала выберите или создайте сборку"));
    // Новый компонент ставится в стороне от уже стоящих, чтобы его было видно и удобно выбирать.
    const double shift = 60.0 * static_cast<double>(document->objectsOf<assembly::Component>().size());
    auto added = context_.assembly().addComponent(*document, partDocumentId, geometry::Placement::translation(shift, 0, shift));
    if (!added) return fail(added.error());
    rebuild();
    view_->fit(view_->camera().yawDegrees, view_->camera().pitchDegrees);
    done(tr("Компонент добавлен."));
    emit projectChanged();
    return true;
}

bool ModelWindow::matePicked(assembly::MateKind kind, double value, bool flip) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return fail(tr("сначала выберите сборку"));
    const auto& picked = view_->pickedFaces();
    if (picked.size() != 2 || picked[0].body == picked[1].body)
        return fail(tr("щёлкните по одной грани на двух разных компонентах"));
    const auto componentOf = [&](int body) { return componentOrder_.at(static_cast<std::size_t>(body)); };
    auto mate = context_.assembly().addMate(
        *document, {kind, {componentOf(picked[0].body), picked[0].name}, {componentOf(picked[1].body), picked[1].name}, value, flip});
    if (!mate) return fail(mate.error());
    auto solved = context_.assembly().solve(*document);
    view_->clearPicked();
    rebuild();
    view_->fit(view_->camera().yawDegrees, view_->camera().pitchDegrees);
    emit projectChanged();
    if (!solved) return fail(solved.error());
    if (!solved.value().solved) return fail(text(solved.value().conflicts.front().second));
    done(tr("Сопряжение добавлено, компоненты расставлены."));
    return true;
}

QString ModelWindow::clearanceText() {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return tr("Зазор измеряется в сборке.");
    const auto& picked = view_->pickedFaces();
    if (picked.size() != 2 || picked[0].body == picked[1].body) return tr("Щёлкните по одной грани на двух разных компонентах.");
    const Uuid first = componentOrder_.at(static_cast<std::size_t>(picked[0].body));
    const Uuid second = componentOrder_.at(static_cast<std::size_t>(picked[1].body));
    auto gap = context_.assembly().clearance(*document, first, second);
    if (!gap) return tr("Не выполнено: ") + text(gap.error().message);
    const auto* a = document->findAs<assembly::Component>(first);
    const auto* b = document->findAs<assembly::Component>(second);
    if (gap.value() < 1e-9)
        return tr("«%1» и «%2» касаются или пересекаются: зазора нет.").arg(text(a->name), text(b->name));
    return tr("Зазор между «%1» и «%2»: %3 мм").arg(text(a->name), text(b->name), number(gap.value(), 3));
}

QString ModelWindow::interferenceReport() {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return tr("Проверка пересечений работает для сборки.");
    auto found = context_.assembly().checkInterference(*document);
    if (!found) return tr("Проверка не выполнена: ") + text(found.error().message);
    if (found.value().empty()) return tr("Пересечений нет.");
    QStringList lines;
    for (const auto& hit : found.value()) {
        const auto* a = document->findAs<assembly::Component>(hit.first);
        const auto* b = document->findAs<assembly::Component>(hit.second);
        lines << tr("«%1» и «%2» пересекаются: общий объём %3 мм³")
                     .arg(text(a ? a->name : std::string("?")), text(b ? b->name : std::string("?")), number(hit.volume, 1));
    }
    return lines.join("\n");
}

QString ModelWindow::billOfMaterialsText() {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return tr("Состав изделия строится для сборки.");
    auto bom = context_.assembly().billOfMaterials(*document);
    if (!bom) return tr("Не выполнено: ") + text(bom.error().message);
    if (bom.value().empty()) return tr("Сборка пуста.");
    QStringList lines;
    double total = 0.0;
    for (const auto& line : bom.value()) {
        lines << tr("%1  %2 — %3 шт., %4 г/шт., %5")
                     .arg(text(line.designation), text(line.name))
                     .arg(line.quantity)
                     .arg(number(line.massGrams, 1), text(line.material));
        total += line.massGrams * line.quantity;
    }
    lines << tr("Общая масса: %1 г").arg(number(total, 1));
    return lines.join("\n");
}

bool ModelWindow::exportBillOfMaterials(const std::filesystem::path& path) {
    Document* document = currentDocument();
    if (!document || document->kind() != DocumentKind::Assembly) return fail(tr("состав изделия строится для сборки"));
    if (auto saved = context_.exportBillOfMaterialsCsv(*document, path); !saved) return fail(saved.error());
    done(tr("Состав изделия сохранён."));
    return true;
}

}  // namespace cosmocad::ui
