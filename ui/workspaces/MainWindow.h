// Главное окно CosmoCad (рабочее пространство «Чертёж»).
#pragma once

#include <QMainWindow>
#include <filesystem>

#include "AppContext.h"
#include "DrawingView.h"
#include "ModelWindow.h"
#include "PropertiesPanel.h"
#include "Ribbon.h"

class QAction;
class QComboBox;
class QTabBar;
class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;

namespace cosmocad::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(app::AppContext& context, QWidget* parent = nullptr);

    void newProjectWithDemo();
    void newEmptyDrawing();
    bool openProject(const std::filesystem::path& path);
    DrawingView* view() const { return view_; }
    QListWidget* layerList() const { return layers_; }
    PropertiesPanel* properties() const { return properties_; }
    QLineEdit* coordinateInput() const { return input_; }
    int runCheck();  // возвращает число замечаний
    // Общий знак шероховатости в правом верхнем углу листа; kind как у DrawingView::placeRoughness.
    bool setSheetRoughness(const QString& parameter, int kind, bool others);
    // Блоки: собрать блок из выбранного (исходные элементы заменяются вставкой блока; базовая точка —
    // левый нижний угол выбранного), вставить блок, разбить выбранную вставку на обычные элементы.
    // Ограничения эскиза. kind — вид ограничения; value — значение для размерных (длина, радиус, угол).
    // Ограничения на один элемент ставятся на каждый выбранный; на два — нужны ровно два выбранных.
    // Возвращает, сколько ограничений поставлено (0 — отказ, причина в строке состояния).
    int constrainSelection(sketcher::ConstraintKind kind, double value = 0.0);
    // То же, но значение — текст: число или формула от параметров детали, выбранной в окне 3D («длина / 2»).
    // С формулой размер следует за параметрами детали.
    int constrainSelection(sketcher::ConstraintKind kind, const QString& valueText);
    // Пересчитывает детали проекта после изменения контуров на чертеже.
    void syncPartsWithDrawing();

private:
    QString lastPluginResult_;        // ответ последней команды дополнения
    std::string pendingExpression_;   // формула размера, который сейчас ставится
    core::Uuid pendingParameters_;    // деталь, чьи параметры она использует

public:
    int removeConstraintsOfSelection();   // снимает все ограничения с выбранных элементов
    bool setConstraintValueOfSelection(double value);  // меняет длину или радиус выбранного элемента
    bool solveConstraints();              // подгоняет геометрию после ручной правки
    QString constraintsText();            // состояние эскиза и список ограничений
    // Дополнения: выполнить команду дополнения над открытым проектом; список дополнений и их состояние.
    bool runPluginCommand(const QString& commandId);
    QString pluginsText();
    // Включает или выключает дополнение; действует со следующего запуска программы.
    bool setPluginEnabled(const QString& pluginId, bool enabled);
    bool makeBlockFromSelection(const QString& name);
    bool insertBlock(const QString& name, double x, double y, double angleDegrees);
    bool explodeSelectedBlock();
    // Технические требования над основной надписью: по пункту на строку; пустой список убирает их.
    bool setTechnicalRequirements(const QStringList& items);
    int staleViewCount();  // сколько видов по модели на открытом чертеже устарело
    // Окно 3D-модели (создаётся при первом обращении).
    ModelWindow* modelWindow();
    QTabBar* drawingTabs() const { return drawings_; }
    Ribbon* ribbon() const { return ribbon_; }

    // Электросхема на открытом листе.
    bool placeSchematicElement(const QString& symbolId, geometry::Point2 at, int rotation, const QString& value);
    bool placeSchematicWire(geometry::Point2 from, geometry::Point2 to);
    bool placeSchematicLabel(geometry::Point2 at, const QString& text);
    int renumberSchematic();     // возвращает число изменённых обозначений
    int numberSchematicNets();   // ставит номера на цепи без имени; возвращает число новых номеров
    int runSchematicCheck();     // возвращает число замечаний; они показываются в панели замечаний
    QString schematicNetsText();
    bool exportElementList(const std::filesystem::path& path);
    int placeElementListOnSheet();  // таблица перечня над основной надписью; возвращает число строк
    bool setSchematicValue(const QString& value);  // номинал выбранного элемента
    bool shiftSchematicElements(double dx, double dy);  // выбранные элементы; проводники едут следом
    // Создаёт в проекте ещё один чертёж и показывает его.
    bool addDrawing(const QString& name);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onNew();
    void onOpen();
    void onSave();
    void onSaveAs();
    void onExport();
    void onUndo();
    void onRedo();
    void onAutosave();
    void onSheetSetup();
    void onAddLayer();
    void onMoveToLayer();
    void onTextRequested(double x, double y);
    void onHatch();
    void onLeaderRequested(double tipX, double tipY, double kneeX, double kneeY);
    void onPrint();
    void onSchematicValue();
    void onSchematicShift();

private:
    void buildUi();
    void showFirstDrawing();
    void refreshActions();
    void refreshLayers();
    bool confirmDiscard();
    bool saveTo(const std::filesystem::path& path);
    bool isModified() const;
    bool askTextAndHeight(const QString& title, QString& text, double& height);
    void report(const core::Error& error);

    app::AppContext& context_;
    DrawingView* view_ = nullptr;
    QListWidget* violations_ = nullptr;
    QListWidget* layers_ = nullptr;
    PropertiesPanel* properties_ = nullptr;
    QLineEdit* input_ = nullptr;
    QTabBar* drawings_ = nullptr;
    Ribbon* ribbon_ = nullptr;
    QComboBox* symbolBox_ = nullptr;
    QComboBox* rotationBox_ = nullptr;
    QWidget* viewButtons_ = nullptr;
    ModelWindow* modelWindow_ = nullptr;
    bool updatingDrawings_ = false;
    QLabel* cursorLabel_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QTimer* autosaveTimer_ = nullptr;
    std::filesystem::path projectPath_;
    bool updatingLayers_ = false;
};

}  // namespace cosmocad::ui
