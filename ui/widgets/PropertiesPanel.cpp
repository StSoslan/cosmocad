#include "PropertiesPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdlib>
#include <string>
#include <cmath>
#include <numbers>

namespace cosmocad::ui {

using namespace drawing;

namespace {

QString typeTitle(const Entity& entity) {
    if (dynamic_cast<const LineEntity*>(&entity)) return PropertiesPanel::tr("Отрезок");
    if (dynamic_cast<const CircleEntity*>(&entity)) return PropertiesPanel::tr("Окружность");
    if (dynamic_cast<const ArcEntity*>(&entity)) return PropertiesPanel::tr("Дуга");
    if (dynamic_cast<const TextEntity*>(&entity)) return PropertiesPanel::tr("Текст");
    if (dynamic_cast<const LinearDimension*>(&entity)) return PropertiesPanel::tr("Линейный размер");
    if (auto* r = dynamic_cast<const RadialDimension*>(&entity))
        return r->diameter ? PropertiesPanel::tr("Размер диаметра") : PropertiesPanel::tr("Размер радиуса");
    if (dynamic_cast<const AngularDimension*>(&entity)) return PropertiesPanel::tr("Угловой размер");
    if (dynamic_cast<const Hatch*>(&entity)) return PropertiesPanel::tr("Штриховка");
    if (dynamic_cast<const Leader*>(&entity)) return PropertiesPanel::tr("Выноска");
    if (dynamic_cast<const RoughnessSign*>(&entity)) return PropertiesPanel::tr("Знак шероховатости");
    if (dynamic_cast<const BlockReference*>(&entity)) return PropertiesPanel::tr("Вставка блока");
    return PropertiesPanel::tr("Элемент");
}

QDoubleSpinBox* makeSpin(QWidget* parent, const char* name, double value, double min, double max, const QString& suffix) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(QString::fromLatin1(name));
    spin->setRange(min, max);
    spin->setDecimals(3);
    spin->setSuffix(suffix);
    spin->setValue(value);
    spin->setKeyboardTracking(false);  // правка применяется по Enter или уходу из поля, а не на каждой цифре
    return spin;
}

}  // namespace

PropertiesPanel::PropertiesPanel(app::AppContext& context, DrawingView* view, QWidget* parent)
    : QWidget(parent), context_(context), view_(view) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(4, 4, 4, 4);
    connect(view_, &DrawingView::selectionChanged, this, &PropertiesPanel::refresh);
    connect(view_, &DrawingView::documentChanged, this, &PropertiesPanel::refresh);
    rebuild();
}

// Панель нельзя перестраивать прямо из сигнала её же поля ввода (поле было бы удалено
// посреди собственного сигнала), поэтому перестройка откладывается до возврата в цикл событий.
void PropertiesPanel::refresh() {
    if (refreshQueued_) return;
    refreshQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        refreshQueued_ = false;
        rebuild();
    });
}

void PropertiesPanel::apply(const EntityEdit& edit) {
    document::Document* document = view_->document();
    if (!document || shown_.isNil()) return;
    if (auto result = context_.drawing().editEntity(*document, shown_, edit); !result)
        emit message(QString::fromStdString(result.error().message));
    refresh();  // при ошибке поле вернётся к действующему значению
}

void PropertiesPanel::rebuild() {
    // Удаляем сразу: rebuild вызывается из отложенного вызова, а не из сигнала поля ввода,
    // поэтому ни одно поле сейчас не выполняет свой обработчик.
    delete content_;
    content_ = new QWidget(this);
    layout_->addWidget(content_);
    auto* form = new QFormLayout(content_);
    form->setContentsMargins(0, 0, 0, 0);
    shown_ = {};

    document::Document* document = view_->document();
    const auto& selection = view_->selection();
    if (!document || selection.size() != 1) {
        auto* label = new QLabel(selection.empty() ? tr("Ничего не выбрано.\nВыберите один элемент, чтобы изменить его свойства.")
                                                   : tr("Выбрано элементов: %1.\nСвойства показываются для одного элемента.")
                                                         .arg(selection.size()),
                                 content_);
        label->setWordWrap(true);
        form->addRow(label);
        return;
    }
    const Entity* entity = document->findAs<Entity>(*selection.begin());
    if (!entity) return;
    shown_ = entity->id();
    auto profile = context_.profileFor(*document);

    auto* type = new QLabel(typeTitle(*entity), content_);
    type->setObjectName(QStringLiteral("prop_type"));
    form->addRow(tr("Элемент"), type);

    // --- Слой ---
    auto* layer = new QComboBox(content_);
    layer->setObjectName(QStringLiteral("prop_layer"));
    layer->addItem(tr("(без слоя)"), QString::fromStdString(core::Uuid().toString()));
    for (const auto* l : document->objectsOf<Layer>()) {
        layer->addItem(QString::fromStdString(l->name), QString::fromStdString(l->id().toString()));
        if (l->id() == entity->layerId) layer->setCurrentIndex(layer->count() - 1);
    }
    form->addRow(tr("Слой"), layer);
    connect(layer, &QComboBox::activated, this, [this, layer] {
        auto id = core::Uuid::parse(layer->currentData().toString().toStdString());
        if (!id || !view_->document()) return;
        if (auto r = context_.drawing().setEntitiesLayer(*view_->document(), {shown_}, id.value()); !r)
            emit message(QString::fromStdString(r.error().message));
    });

    // --- Общие заготовки ---
    auto addText = [&](const std::string& value) {
        auto* edit = new QLineEdit(QString::fromStdString(value), content_);
        edit->setObjectName(QStringLiteral("prop_text"));
        form->addRow(tr("Текст"), edit);
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, value] {
            if (edit->text().toStdString() == value) return;
            EntityEdit e;
            e.text = edit->text().toStdString();
            apply(e);
        });
    };
    auto addHeight = [&](double value) {
        auto* combo = new QComboBox(content_);
        combo->setObjectName(QStringLiteral("prop_height"));
        bool listed = false;
        if (profile) {
            for (double h : profile->textHeights) {
                combo->addItem(QString::number(h), h);
                if (std::abs(h - value) < 1e-9) {
                    combo->setCurrentIndex(combo->count() - 1);
                    listed = true;
                }
            }
        }
        if (!listed) {  // нестандартная высота: показываем её, чтобы было видно, что исправлять
            combo->addItem(tr("%1 (не по стандарту)").arg(value), value);
            combo->setCurrentIndex(combo->count() - 1);
        }
        form->addRow(tr("Высота шрифта, мм"), combo);
        connect(combo, &QComboBox::activated, this, [this, combo] {
            EntityEdit e;
            e.textHeight = combo->currentData().toDouble();
            apply(e);
        });
    };
    auto addNote = [&](const DimensionNote& note) {
        struct Field {
            const char* name;
            QString title;
            std::string DimensionNote::* member;
            QString hint;
        };
        const Field fields[] = {
            {"prop_suffix", tr("Допуск / посадка"), &DimensionNote::suffix, tr("например: H7 или ±0,1")},
            {"prop_upper", tr("Верхнее отклонение"), &DimensionNote::upper, tr("например: +0,2")},
            {"prop_lower", tr("Нижнее отклонение"), &DimensionNote::lower, tr("например: −0,1")},
        };
        for (const Field& field : fields) {
            auto* edit = new QLineEdit(QString::fromStdString(note.*field.member), content_);
            edit->setObjectName(QString::fromLatin1(field.name));
            edit->setPlaceholderText(field.hint);
            form->addRow(field.title, edit);
            auto member = field.member;
            connect(edit, &QLineEdit::editingFinished, this, [this, edit, member] {
                // Берём действующее состояние из документа, чтобы не затереть соседние поля.
                document::Document* doc = view_->document();
                if (!doc) return;
                DimensionNote current;
                if (auto* l = doc->findAs<LinearDimension>(shown_)) current = l->note;
                else if (auto* r = doc->findAs<RadialDimension>(shown_)) current = r->note;
                else return;
                std::string value = edit->text().trimmed().toStdString();
                if (value == current.*member) return;
                current.*member = value;
                EntityEdit e;
                e.note = current;
                apply(e);
            });
        }
    };

    // --- Свойства по виду элемента ---
    if (auto* line = dynamic_cast<const LineEntity*>(entity)) {
        auto* length = new QLabel(tr("%1 мм").arg(geometry::distance(line->start, line->end), 0, 'f', 3), content_);
        length->setObjectName(QStringLiteral("prop_length"));
        form->addRow(tr("Длина"), length);
    } else if (auto* circle = dynamic_cast<const CircleEntity*>(entity)) {
        auto* spin = makeSpin(content_, "prop_radius", circle->radius, 0.001, 1e6, tr(" мм"));
        form->addRow(tr("Радиус"), spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            EntityEdit e;
            e.radius = v;
            apply(e);
        });
    } else if (auto* arc = dynamic_cast<const ArcEntity*>(entity)) {
        auto* spin = makeSpin(content_, "prop_radius", arc->radius, 0.001, 1e6, tr(" мм"));
        form->addRow(tr("Радиус"), spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            EntityEdit e;
            e.radius = v;
            apply(e);
        });
    } else if (auto* text = dynamic_cast<const TextEntity*>(entity)) {
        addText(text->text);
        addHeight(text->height);
    } else if (auto* leader = dynamic_cast<const Leader*>(entity)) {
        addText(leader->text);
        addHeight(leader->textHeight);
    } else if (auto* linear = dynamic_cast<const LinearDimension*>(entity)) {
        auto* value = new QLabel(tr("%1 мм").arg(linear->value(), 0, 'f', 3), content_);
        form->addRow(tr("Значение"), value);
        auto* spin = makeSpin(content_, "prop_offset", linear->offset, -1e6, 1e6, tr(" мм"));
        form->addRow(tr("Отступ размерной линии"), spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            EntityEdit e;
            e.offset = v;
            apply(e);
        });
        addHeight(linear->textHeight);
        addNote(linear->note);
    } else if (auto* radial = dynamic_cast<const RadialDimension*>(entity)) {
        auto* box = new QCheckBox(tr("Показывать диаметр"), content_);
        box->setObjectName(QStringLiteral("prop_diameter"));
        box->setChecked(radial->diameter);
        form->addRow(QString(), box);
        connect(box, &QCheckBox::toggled, this, [this](bool on) {
            EntityEdit e;
            e.diameter = on;
            apply(e);
        });
        addHeight(radial->textHeight);
        addNote(radial->note);
    } else if (auto* angular = dynamic_cast<const AngularDimension*>(entity)) {
        form->addRow(tr("Значение"), new QLabel(tr("%1°").arg(angular->valueDegrees(), 0, 'f', 3), content_));
        addHeight(angular->textHeight);
    } else if (auto* hatch = dynamic_cast<const Hatch*>(entity)) {
        auto* spacing = makeSpin(content_, "prop_spacing", hatch->spacing, 0.05, 1000.0, tr(" мм"));
        form->addRow(tr("Расстояние между линиями"), spacing);
        connect(spacing, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            EntityEdit e;
            e.hatchSpacing = v;
            apply(e);
        });
        auto* angle = makeSpin(content_, "prop_angle", hatch->angle * 180.0 / std::numbers::pi, -360.0, 360.0, tr("°"));
        form->addRow(tr("Наклон линий"), angle);
        connect(angle, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            EntityEdit e;
            e.hatchAngleDegrees = v;
            apply(e);
        });
    }
}

}  // namespace cosmocad::ui
