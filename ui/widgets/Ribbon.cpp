#include "Ribbon.h"

#include <QAction>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <utility>
#include <vector>
#include <algorithm>
#include <functional>
#include <map>

namespace cosmocad::ui {

namespace {

const QColor kInk(0x2B, 0x33, 0x40);     // основной цвет линий значка
const QColor kAccent(0x1F, 0x6F, 0xEB);  // синий акцент

using Painter = std::function<void(QPainter&)>;

// Значки рисуются в поле 32×32; перо — тёмное, акцентные точки и детали — синие.
const std::map<QString, Painter>& painters() {
    static const auto dot = [](QPainter& p, double x, double y) {
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(kAccent);
        p.drawEllipse(QPointF(x, y), 2.6, 2.6);
        p.restore();
    };
    static const auto accent = [](QPainter& p) { p.setPen(QPen(kAccent, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); };
    static const auto arrow = [](QPainter& p, QPointF tip, QPointF from) {
        const QLineF l(tip, from);
        QLineF a = l, b = l;
        a.setLength(5);
        b.setLength(5);
        a.setAngle(l.angle() + 28);
        b.setAngle(l.angle() - 28);
        p.drawLine(l);
        p.drawLine(a);
        p.drawLine(b);
    };
    static const std::map<QString, Painter> map = {
        {"Выбор", [](QPainter& p) { p.drawPolygon(QPolygonF({{9, 5}, {9, 25}, {14, 20}, {18, 28}, {21, 26}, {17, 19}, {24, 19}})); }},
        {"Отрезок", [](QPainter& p) { p.drawLine(QPointF(6, 26), QPointF(26, 6)); dot(p, 6, 26); dot(p, 26, 6); }},
        {"Прямоугольник", [](QPainter& p) { p.drawRect(QRectF(5, 8, 22, 16)); dot(p, 5, 24); dot(p, 27, 8); }},
        {"Окружность", [](QPainter& p) { p.drawEllipse(QPointF(16, 16), 11, 11); p.drawLine(QPointF(16, 16), QPointF(27, 16)); dot(p, 16, 16); }},
        {"Дуга", [](QPainter& p) { p.drawArc(QRectF(6, 8, 36, 36), 90 * 16, 90 * 16); dot(p, 6, 26); dot(p, 24, 8); }},
        {"Текст", [](QPainter& p) { p.drawLine(QPointF(7, 27), QPointF(16, 5)); p.drawLine(QPointF(16, 5), QPointF(25, 27)); p.drawLine(QPointF(10.5, 19), QPointF(21.5, 19)); }},
        {"Размер", [](QPainter& p) { p.drawLine(QPointF(5, 6), QPointF(5, 26)); p.drawLine(QPointF(27, 6), QPointF(27, 26)); accent(p); arrow(p, {5, 16}, {16, 16}); arrow(p, {27, 16}, {16, 16}); }},
        {"Радиус", [](QPainter& p) { p.drawArc(QRectF(4, 4, 24, 24), 0, 300 * 16); accent(p); arrow(p, {24.5, 7.5}, {16, 16}); }},
        {"Диаметр", [](QPainter& p) { p.drawEllipse(QPointF(16, 16), 11, 11); accent(p); p.drawLine(QPointF(7, 25), QPointF(25, 7)); }},
        {"Выноска", [](QPainter& p) { arrow(p, {6, 26}, {17, 11}); p.drawLine(QPointF(17, 11), QPointF(27, 11)); dot(p, 6, 26); }},
        {"Угол", [](QPainter& p) { p.drawLine(QPointF(5, 26), QPointF(27, 26)); p.drawLine(QPointF(5, 26), QPointF(22, 7)); accent(p); p.drawArc(QRectF(-9, 12, 28, 28), 0, 48 * 16); }},
        {"Штриховка", [](QPainter& p) { p.drawRect(QRectF(5, 5, 22, 22)); accent(p); for (int i = 0; i < 4; ++i) p.drawLine(QPointF(5 + i * 5.5, 27), QPointF(5 + i * 5.5 + 5.5, 21.5 - 0)); p.drawLine(QPointF(5, 21), QPointF(21, 5)); p.drawLine(QPointF(5, 13), QPointF(13, 5)); p.drawLine(QPointF(11, 27), QPointF(27, 11)); p.drawLine(QPointF(19, 27), QPointF(27, 19)); }},
        {"Перенос", [](QPainter& p) { arrow(p, {16, 4}, {16, 16}); arrow(p, {16, 28}, {16, 16}); arrow(p, {4, 16}, {16, 16}); arrow(p, {28, 16}, {16, 16}); }},
        {"Копия", [](QPainter& p) { p.drawRoundedRect(QRectF(5, 5, 15, 15), 2, 2); accent(p); p.drawRoundedRect(QRectF(12, 12, 15, 15), 2, 2); }},
        {"Поворот", [](QPainter& p) { p.drawArc(QRectF(6, 6, 20, 20), 40 * 16, 280 * 16); arrow(p, {23.5, 9.5}, {22, 15}); dot(p, 16, 16); }},
        {"Отражение", [](QPainter& p) { p.drawPolygon(QPolygonF({{13, 8}, {13, 24}, {4, 24}})); accent(p); p.drawPolygon(QPolygonF({{19, 8}, {19, 24}, {28, 24}})); p.setPen(QPen(kInk, 1.2, Qt::DashLine)); p.drawLine(QPointF(16, 4), QPointF(16, 28)); }},
        {"Обрезка", [](QPainter& p) { p.drawLine(QPointF(4, 22), QPointF(28, 22)); p.drawLine(QPointF(10, 5), QPointF(10, 22)); accent(p); p.setPen(QPen(kAccent, 2, Qt::DotLine)); p.drawLine(QPointF(10, 22), QPointF(10, 29)); }},
        {"Скругление", [](QPainter& p) { p.drawLine(QPointF(5, 27), QPointF(5, 15)); p.drawLine(QPointF(15, 5), QPointF(27, 5)); accent(p); p.drawArc(QRectF(5, 5, 20, 20), 90 * 16, 90 * 16); }},
        {"Фаска", [](QPainter& p) { p.drawLine(QPointF(5, 27), QPointF(5, 15)); p.drawLine(QPointF(15, 5), QPointF(27, 5)); accent(p); p.drawLine(QPointF(5, 15), QPointF(15, 5)); }},
        {"Слой", [](QPainter& p) { p.drawPolygon(QPolygonF({{16, 14}, {28, 20}, {16, 26}, {4, 20}})); p.setBrush(kAccent); accent(p); p.drawPolygon(QPolygonF({{16, 5}, {28, 11}, {16, 17}, {4, 11}})); }},
        {"Проверка", [](QPainter& p) { p.drawRoundedRect(QRectF(5, 5, 22, 22), 3, 3); accent(p); p.setPen(QPen(kAccent, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPolyline(QPolygonF({{10, 16}, {15, 21}, {23, 10}})); }},
        {"Лист", [](QPainter& p) { p.drawRect(QRectF(7, 4, 18, 24)); accent(p); p.drawRect(QRectF(15, 21, 10, 7)); }},
        {"3D", [](QPainter& p) { p.drawPolygon(QPolygonF({{16, 4}, {27, 10}, {27, 22}, {16, 28}, {5, 22}, {5, 10}})); accent(p); p.drawLine(QPointF(5, 10), QPointF(16, 16)); p.drawLine(QPointF(27, 10), QPointF(16, 16)); p.drawLine(QPointF(16, 16), QPointF(16, 28)); }},
        {"Обновить", [](QPainter& p) { p.drawArc(QRectF(6, 6, 20, 20), 30 * 16, 150 * 16); p.drawArc(QRectF(6, 6, 20, 20), 210 * 16, 150 * 16); accent(p); arrow(p, {24.6, 11}, {19, 11}); arrow(p, {7.4, 21}, {13, 21}); }},
        {"Элемент", [](QPainter& p) { p.drawLine(QPointF(3, 16), QPointF(9, 16)); p.drawLine(QPointF(23, 16), QPointF(29, 16)); accent(p); p.drawRect(QRectF(9, 11, 14, 10)); }},
        {"Проводник", [](QPainter& p) { p.drawPolyline(QPolygonF({{5, 25}, {5, 12}, {27, 12}, {27, 5}})); dot(p, 16, 12); accent(p); p.drawLine(QPointF(16, 12), QPointF(16, 27)); }},
        {"Метка", [](QPainter& p) { p.drawPolygon(QPolygonF({{5, 10}, {20, 10}, {27, 16}, {20, 22}, {5, 22}})); dot(p, 11, 16); }},
        {"Номера", [](QPainter& p) { QFont f = p.font(); f.setPixelSize(13); f.setBold(true); p.setFont(f); p.drawText(QRectF(2, 3, 28, 14), Qt::AlignCenter, "R1"); p.setPen(kAccent); p.drawText(QRectF(2, 16, 28, 14), Qt::AlignCenter, "R2"); }},
        {"Цепи", [](QPainter& p) { p.drawLine(QPointF(8, 8), QPointF(24, 8)); p.drawLine(QPointF(8, 8), QPointF(8, 24)); p.drawLine(QPointF(24, 8), QPointF(24, 24)); dot(p, 8, 8); dot(p, 24, 8); dot(p, 8, 24); dot(p, 24, 24); }},
        {"Таблица", [](QPainter& p) { p.drawRect(QRectF(5, 6, 22, 20)); p.drawLine(QPointF(5, 13), QPointF(27, 13)); p.drawLine(QPointF(5, 19.5), QPointF(27, 19.5)); accent(p); p.drawLine(QPointF(13, 6), QPointF(13, 26)); }},
        {"Новый", [](QPainter& p) { p.drawPolygon(QPolygonF({{8, 4}, {19, 4}, {25, 10}, {25, 28}, {8, 28}})); accent(p); p.drawLine(QPointF(16.5, 14), QPointF(16.5, 24)); p.drawLine(QPointF(11.5, 19), QPointF(21.5, 19)); }},
        {"Открыть", [](QPainter& p) { p.drawPolygon(QPolygonF({{4, 26}, {4, 7}, {12, 7}, {15, 10}, {26, 10}, {26, 26}})); accent(p); p.drawLine(QPointF(4, 15), QPointF(26, 15)); }},
        {"Сохранить", [](QPainter& p) { p.drawPolygon(QPolygonF({{5, 5}, {22, 5}, {27, 10}, {27, 27}, {5, 27}})); p.drawRect(QRectF(10, 5, 10, 7)); accent(p); p.drawRect(QRectF(10, 17, 12, 10)); }},
        {"Экспорт", [](QPainter& p) { p.drawPolyline(QPolygonF({{6, 18}, {6, 27}, {26, 27}, {26, 18}})); accent(p); arrow(p, {16, 4}, {16, 20}); }},
        {"Печать", [](QPainter& p) { p.drawRect(QRectF(9, 4, 14, 7)); p.drawRoundedRect(QRectF(4, 11, 24, 11), 2, 2); accent(p); p.drawRect(QRectF(9, 18, 14, 10)); }},
        {"Отменить", [](QPainter& p) { p.drawArc(QRectF(7, 9, 18, 16), -90 * 16, 250 * 16); arrow(p, {6, 12}, {11.5, 15.5}); }},
        {"Повторить", [](QPainter& p) { p.drawArc(QRectF(7, 9, 18, 16), 20 * 16, 250 * 16); arrow(p, {26, 12}, {20.5, 15.5}); }},
        {"Показать всё", [](QPainter& p) { for (const auto& c : {QPointF(5, 5), QPointF(27, 5), QPointF(5, 27), QPointF(27, 27)}) { const double sx = c.x() < 16 ? 1 : -1, sy = c.y() < 16 ? 1 : -1; p.drawLine(c, c + QPointF(7 * sx, 0)); p.drawLine(c, c + QPointF(0, 7 * sy)); } accent(p); p.drawRect(QRectF(11, 11, 10, 10)); }},
        {"Ближе", [](QPainter& p) { p.drawEllipse(QPointF(14, 14), 9, 9); p.drawLine(QPointF(21, 21), QPointF(28, 28)); accent(p); p.drawLine(QPointF(10, 14), QPointF(18, 14)); p.drawLine(QPointF(14, 10), QPointF(14, 18)); }},
        {"Дальше", [](QPainter& p) { p.drawEllipse(QPointF(14, 14), 9, 9); p.drawLine(QPointF(21, 21), QPointF(28, 28)); accent(p); p.drawLine(QPointF(10, 14), QPointF(18, 14)); }},
        {"Удалить", [](QPainter& p) { p.drawLine(QPointF(6, 9), QPointF(26, 9)); p.drawLine(QPointF(12, 9), QPointF(13, 5)); p.drawLine(QPointF(13, 5), QPointF(19, 5)); p.drawLine(QPointF(19, 5), QPointF(20, 9)); p.drawPolygon(QPolygonF({{8, 9}, {24, 9}, {22, 28}, {10, 28}})); }},
        {"Параметр", [](QPainter& p) { QFont f = p.font(); f.setPixelSize(17); f.setItalic(true); p.setFont(f); p.drawText(QRectF(2, 4, 16, 24), Qt::AlignCenter, "x"); accent(p); p.drawLine(QPointF(19, 13), QPointF(28, 13)); p.drawLine(QPointF(19, 19), QPointF(28, 19)); }},
        {"Выдавить", [](QPainter& p) { p.drawPolygon(QPolygonF({{5, 22}, {16, 27}, {27, 22}, {16, 17}})); accent(p); arrow(p, {16, 3}, {16, 17}); }},
        {"Вырезать", [](QPainter& p) { p.drawPolygon(QPolygonF({{5, 10}, {16, 15}, {27, 10}, {16, 5}})); accent(p); arrow(p, {16, 28}, {16, 12}); }},
        {"Компонент", [](QPainter& p) { p.drawRect(QRectF(4, 14, 14, 14)); accent(p); p.drawRect(QRectF(14, 4, 14, 14)); }},
        {"Сопряжение", [](QPainter& p) { p.drawRect(QRectF(4, 8, 11, 16)); p.drawRect(QRectF(17, 8, 11, 16)); accent(p); p.drawLine(QPointF(16, 4), QPointF(16, 28)); }},
        {"Соосность", [](QPainter& p) { p.drawEllipse(QPointF(16, 16), 11, 11); accent(p); p.drawEllipse(QPointF(16, 16), 5, 5); }},
    };
    return map;
}

// Название команды → какой значок рисовать. Ключевое слово ищется в названии.
QString iconKeyFor(const QString& title) {
    static const std::vector<std::pair<QString, QString>> rules = {
        {"Скругл", "Скругление"}, {"Фаск", "Фаска"}, {"Выдав", "Выдавить"}, {"Вырез", "Вырезать"}, {"Параметр", "Параметр"},
        {"Новая деталь", "3D"}, {"Новая сборка", "Компонент"}, {"компонент", "Компонент"}, {"Совпадение", "Сопряжение"},
        {"Заподлицо", "Сопряжение"}, {"Соосность", "Соосность"}, {"Расстояние", "Размер"}, {"Пересечения", "Проверка"},
        {"Состав", "Таблица"}, {"Перечень", "Таблица"}, {"CSV", "Таблица"}, {"STEP", "Экспорт"}, {"STL", "Экспорт"},
        {"Вид на чертёж", "Лист"}, {"Обновить", "Обновить"}, {"Спереди", "3D"}, {"Сверху", "3D"}, {"Слева", "3D"},
        {"Изометрия", "3D"}, {"3D", "3D"}, {"Провер", "Проверка"}, {"Лист", "Лист"}, {"Новый слой", "Слой"},
        {"слой", "Слой"}, {"Перенумер", "Номера"}, {"Номинал", "Текст"}, {"Сдвинуть", "Перенос"}, {"Цепи", "Цепи"},
        {"Новый", "Новый"}, {"Пример", "Открыть"}, {"Открыть", "Открыть"}, {"Сохранить", "Сохранить"},
        {"Экспорт", "Экспорт"}, {"Печать", "Печать"}, {"Отменить", "Отменить"}, {"Повторить", "Повторить"},
        {"Удалить", "Удалить"}, {"Метка", "Метка"},
    };
    if (painters().count(title)) return title;
    for (const auto& [needle, key] : rules)
        if (title.contains(needle)) return key;
    return QString();
}

}  // namespace

QIcon commandIcon(const QString& title) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);
    painter.setPen(QPen(kInk, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const QString key = iconKeyFor(title);
    if (const auto it = painters().find(key); it != painters().end()) {
        it->second(painter);
    } else {
        painter.drawRoundedRect(QRectF(7, 7, 18, 18), 4, 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kAccent);
        painter.drawEllipse(QPointF(16, 16), 3, 3);
    }
    return QIcon(pixmap);
}

QString applicationStyle() {
    return QStringLiteral(R"(
QMainWindow, QDialog { background: #F2F4F8; }
QWidget { color: #2B3340; font-size: 13px; }
#ribbon { background: #FFFFFF; border: 1px solid #E3E7EE; border-radius: 12px; }
#ribbonTabs { background: transparent; }
QTabBar::tab { background: transparent; border: none; padding: 7px 14px; margin: 0 2px; color: #4A5565; }
QTabBar::tab:selected { color: #1F6FEB; border-bottom: 2px solid #1F6FEB; }
QTabBar::tab:hover { color: #1F6FEB; }
#ribbonGroupTitle { color: #7A8494; font-size: 11px; }
#ribbonSeparator { background: #E3E7EE; }
QToolButton { background: transparent; border: none; border-radius: 8px; padding: 4px 6px; }
QToolButton:hover { background: #EAF1FE; }
QToolButton:checked, QToolButton:pressed { background: #D7E5FD; }
QToolBar { background: transparent; border: none; spacing: 4px; }
QDockWidget { titlebar-close-icon: none; font-weight: 600; }
QDockWidget::title { background: transparent; padding: 8px 10px 4px 10px; }
QDockWidget > QWidget { background: #FFFFFF; border: 1px solid #E3E7EE; border-radius: 12px; }
QListWidget, QTableWidget, QTreeView { background: #FFFFFF; border: 1px solid #E3E7EE; border-radius: 8px; }
QListWidget::item:selected, QTableWidget::item:selected { background: #D7E5FD; color: #1B2533; }
QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox { background: #FFFFFF; border: 1px solid #D5DBE5; border-radius: 8px; padding: 4px 8px; }
QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus { border: 1px solid #1F6FEB; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView { background: #FFFFFF; border: 1px solid #D5DBE5; selection-background-color: #D7E5FD; selection-color: #1B2533; }
#commandLine { border-radius: 14px; padding: 6px 14px; min-height: 18px; }
QPushButton { background: #FFFFFF; border: 1px solid #D5DBE5; border-radius: 8px; padding: 5px 12px; }
QPushButton:hover { background: #EAF1FE; border-color: #1F6FEB; }
QStatusBar { background: transparent; color: #5B6575; }
QHeaderView::section { background: #F6F8FB; border: none; border-bottom: 1px solid #E3E7EE; padding: 4px; }
#viewButtons { background: #FFFFFF; border: 1px solid #E3E7EE; border-radius: 10px; }
#bottomBar { background: transparent; }
)");
}

// ---- Группа ----

RibbonGroup::RibbonGroup(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 2);
    layout->setSpacing(2);
    grid_ = new QGridLayout();
    grid_->setSpacing(2);
    layout->addLayout(grid_, 1);
    auto* label = new QLabel(title, this);
    label->setObjectName("ribbonGroupTitle");
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
}

QToolButton* RibbonGroup::addAction(QAction* action) {
    auto* button = new QToolButton(this);
    if (action->icon().isNull()) action->setIcon(commandIcon(action->text()));
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    if (!action->objectName().isEmpty()) button->setObjectName("btn_" + action->objectName());
    if (compact_) {
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(18, 18));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        grid_->addWidget(button, row_, column_);
        if (++row_ == 3) {
            row_ = 0;
            ++column_;
        }
    } else {
        if (row_ != 0) {  // после мелких кнопок крупная начинает новый столбец
            row_ = 0;
            ++column_;
        }
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(30, 30));
        // Подпись показывается целиком: ширина кнопки — по самому длинному слову подписи.
        button->setMinimumWidth(std::max(58, button->fontMetrics().horizontalAdvance(action->iconText()) * 23 / 20 + 26));
        grid_->addWidget(button, 0, column_++, 3, 1);
    }
    return button;
}

QAction* RibbonGroup::addAction(const QString& title) {
    auto* action = new QAction(title, this);
    addAction(action);
    return action;
}

void RibbonGroup::addWidget(QWidget* widget) {
    if (row_ != 0) {
        row_ = 0;
        ++column_;
    }
    grid_->addWidget(widget, 0, column_++, 3, 1, Qt::AlignVCenter);
}

// ---- Лента ----

Ribbon::Ribbon(QWidget* parent) : QWidget(parent) {
    setObjectName("ribbon");
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 2, 8, 4);
    layout->setSpacing(0);
    top_ = new QHBoxLayout();
    tabs_ = new QTabBar(this);
    tabs_->setObjectName("ribbonTabs");
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    top_->addWidget(tabs_);
    top_->addStretch(1);
    layout->addLayout(top_);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_);
    connect(tabs_, &QTabBar::currentChanged, pages_, &QStackedWidget::setCurrentIndex);
}

int Ribbon::addPage(const QString& title) {
    auto* page = new QWidget(pages_);
    auto* layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 2, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1);
    pages_->addWidget(page);
    return tabs_->addTab(title);
}

RibbonGroup* Ribbon::addGroup(int page, const QString& title) {
    QWidget* host = pages_->widget(page);
    auto* layout = static_cast<QHBoxLayout*>(host->layout());
    auto* group = new RibbonGroup(title, host);
    const int position = layout->count() - 1;  // перед растяжкой в конце
    if (position > 0) {
        auto* line = new QFrame(host);
        line->setObjectName("ribbonSeparator");
        line->setFixedWidth(1);
        layout->insertWidget(position, line);
        layout->insertWidget(position + 1, group);
    } else {
        layout->insertWidget(position, group);
    }
    return group;
}

void Ribbon::setCurrentPage(int page) { tabs_->setCurrentIndex(page); }
int Ribbon::currentPage() const { return tabs_->currentIndex(); }
int Ribbon::pageCount() const { return tabs_->count(); }

void Ribbon::setCornerWidget(QWidget* widget) { top_->addWidget(widget); }

}  // namespace cosmocad::ui
