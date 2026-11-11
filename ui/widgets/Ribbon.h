// Лента команд: вкладки сверху, на каждой — группы крупных и мелких кнопок с подписями.
// Значки рисуются программой (без файлов картинок), стиль окна — светлый, со скруглёнными панелями.
#pragma once

#include <QIcon>
#include <QWidget>

class QAction;
class QGridLayout;
class QHBoxLayout;
class QStackedWidget;
class QTabBar;
class QToolButton;

namespace cosmocad::ui {

// Значок команды по её названию. Для неизвестного названия — нейтральный значок.
QIcon commandIcon(const QString& title);

// Общий стиль окон CosmoCad.
QString applicationStyle();

class RibbonGroup : public QWidget {
    Q_OBJECT
public:
    explicit RibbonGroup(const QString& title, QWidget* parent = nullptr);

    // Следующие кнопки — мелкие (значок и подпись в строку, по три в столбце).
    void setCompact(bool compact) { compact_ = compact; }
    // Кнопка для готовой команды; значок берётся по названию команды.
    QToolButton* addAction(QAction* action);
    // Создаёт команду и кнопку для неё.
    QAction* addAction(const QString& title);
    void addWidget(QWidget* widget);
    void addSeparator() {}  // группы сами отделены друг от друга

private:
    QGridLayout* grid_ = nullptr;
    bool compact_ = false;
    int column_ = 0, row_ = 0;
};

class Ribbon : public QWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget* parent = nullptr);

    int addPage(const QString& title);
    RibbonGroup* addGroup(int page, const QString& title);
    void setCurrentPage(int page);
    int currentPage() const;
    int pageCount() const;
    // Виджет в правой части строки вкладок (например, поле команды).
    void setCornerWidget(QWidget* widget);

private:
    QTabBar* tabs_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QHBoxLayout* top_ = nullptr;
};

}  // namespace cosmocad::ui
