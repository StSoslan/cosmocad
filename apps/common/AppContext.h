// Общая «сборка» модулей для приложений: связывает хранилище, сервисы и профили стандартов.
// Не зависит от Qt — используется и оконным приложением, и консольной утилитой, и тестами.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include "cosmocad/assembly/AssemblyService.h"
#include "cosmocad/document/IDocumentService.h"
#include "cosmocad/drawing/IDrawingService.h"
#include "cosmocad/geometry/IGeometryKernel.h"
#include "cosmocad/modeling/ModelingService.h"
#include "Plugins.h"
#include "cosmocad/libraries/Fasteners.h"
#include "cosmocad/libraries/Materials.h"
#include "cosmocad/libraries/Profiles.h"
#include "cosmocad/piping/Pipes.h"
#include "cosmocad/schematic/Schematic.h"
#include "cosmocad/sketcher/Sketcher.h"
#include "cosmocad/standards/Engine.h"
#include "cosmocad/storage/ProjectStorage.h"

namespace cosmocad::app {

// Каталог ресурсов: переменная окружения COSMOCAD_RESOURCES, затем каталог рядом
// с программой, затем каталог исходников (для запуска из дерева сборки).
std::filesystem::path findResourceDirectory(const std::filesystem::path& executableDirectory);

class AppContext {
public:
    // userProfileDirectory — необязательный каталог пользовательских профилей;
    // они перекрывают поставляемые профили с тем же id.
    static core::Result<std::unique_ptr<AppContext>> create(
        const std::filesystem::path& resourceDirectory,
        const std::optional<std::filesystem::path>& userProfileDirectory = std::nullopt);

    document::IDocumentService& documents() { return *documents_; }
    drawing::IDrawingService& drawing() { return drawing_; }
    standards::IStandardsService& standards() { return *standards_; }
    storage::ProjectStorage& storage() { return *storage_; }

    // Профиль документа; пусто, если профиль не выбран или не найден.
    std::optional<standards::Profile> profileFor(const document::Document& document) const;

    // Создаёт в проекте пример чертежа (пластина с отверстием) по профилю ЕСКД.
    core::Result<document::Document*> createDemoDrawing();

    // ---- Электросхемы ----
    schematic::SchematicService& schematic() { return *schematic_; }
    sketcher::SketchService& sketcher() { return sketcher_; }
    const libraries::FastenerLibrary& fasteners() const { return *fasteners_; }
    // Материалы: встроенные и свои. Свои хранятся в папке пользователя (её можно задать переменной
    // окружения COSMOCAD_USER_DIR) и доступны во всех проектах.
    libraries::MaterialLibrary& materials() { return *materials_; }
    // Прокат: уголки. Отрезок уголка как деталь проекта; такая деталь уже есть — возвращается она.
    const libraries::ProfileLibrary& profiles() const { return *profiles_; }
    core::Result<document::Document*> anglePart(double b, double t, double length);
    // Отрезок швеллера (kind = "channel") или двутавра ("beam") по номеру профиля.
    core::Result<document::Document*> profilePart(const std::string& kind, const std::string& name, double length);
    // Трубы из полипропилена: расчёт и размерный ряд.
    const piping::PipeLibrary& pipes() const { return *pipes_; }
    // Отрезок трубы как деталь проекта; такая деталь уже есть — возвращается она.
    core::Result<document::Document*> pipePart(const std::string& material, double diameter, double wall, double length);
    // Дополнения. Программа после запуска вызывает loadPlugins(); тесты загружают нужную папку сами.
    PluginManager& plugins() { return *plugins_; }
    // Загружает дополнения из обычных мест: папка plugins рядом с программой, папка пользователя
    // и папка из переменной COSMOCAD_PLUGIN_DIR. Выключенные перечислены в файле plugins-disabled.txt
    // в папке пользователя (по имени на строку). Возвращает число активных дополнений.
    int loadPlugins(const std::filesystem::path& executableDirectory);
    // Дополнения, выключенные пользователем, и переключение. Изменение действует со следующего запуска.
    std::vector<std::string> disabledPlugins() const;
    core::Result<void> setPluginDisabled(const std::string& pluginId, bool disabled);
    // Назначает детали материал из библиотеки: название в основную надпись и плотность для массы.
    core::Result<void> setPartMaterial(document::Document& part, const std::string& materialTitle);
    // Стандартное изделие из библиотеки крепежа как деталь проекта. Если такая деталь в проекте уже есть,
    // возвращается она: одно изделие — один документ, сколько бы раз его ни вставляли в сборки.
    core::Result<document::Document*> standardPart(const libraries::FastenerRequest& request);
    // Создаёт в проекте пример схемы (фильтр из двух резисторов и конденсатора) на листе по ЕСКД.
    core::Result<document::Document*> createDemoSchematic();

    // Таблица «Перечень элементов» на листе схемы по ГОСТ 2.701-2008, п. 5.7: графы 20, 110, 10
    // и 45 мм, головка 15 мм, строки по 8 мм; таблица стоит над основной надписью с зазором 12 мм.
    // Прежняя таблица на этом листе заменяется. Возвращает число строк.
    core::Result<int> placeElementListTable(document::Document& document);

    // ---- 3D ----
    geometry::IGeometryKernel& kernel() { return *kernel_; }
    modeling::IModelingService& modeling() { return *modeling_; }
    assembly::IAssemblyService& assembly() { return *assembly_; }

    // Тела документа: у детали — одно тело, у сборки — тела компонентов на своих местах.
    // Деталь при необходимости перестраивается.
    // exploded — компоненты сборки на местах разнесённого вида.
    core::Result<std::vector<geometry::ShapePtr>> bodiesOf(document::Document& document, bool exploded = false);
    // Перестраивает все детали проекта (после открытия файла: тела в файле не хранятся).
    core::Result<void> rebuildAll();

    // Вид детали или сборки на чертеже: front, top, left, right, back, bottom.
    core::Result<core::Uuid> addModelView(document::Document& drawingDocument, document::Document& source,
                                          const std::string& view, geometry::Point2 center, double scale,
                                          bool showHidden, bool exploded = false);
    // Разрез: то же, что вид, но часть модели перед секущей плоскостью убрана, а срез заштрихован.
    // Плоскость перпендикулярна взгляду; at — её координата вдоль направления взгляда, без неё плоскость
    // проходит через середину модели. Над разрезом ставится надпись («А–А», «Б–Б»…), а на подходящем
    // виде той же модели, если он есть на листе, — штрихи секущей плоскости со стрелками и буквами.
    core::Result<core::Uuid> addSectionView(document::Document& drawingDocument, document::Document& source,
                                            const std::string& view, geometry::Point2 center, double scale,
                                            std::optional<double> at = std::nullopt, bool onlyCut = false);
    // Переносит секущую плоскость готового разреза на новую координату; разрез перестраивается на месте.
    core::Result<void> moveSection(document::Document& drawingDocument, const core::Uuid& viewId, double at);
    // Сколько видов чертежа устарело: модель изменилась, а вид ещё не обновлён.
    core::Result<int> staleViewCount(document::Document& drawingDocument);
    // Обновляет все виды чертежа по текущему состоянию моделей. Возвращает число обновлённых видов.
    core::Result<int> updateModelViews(document::Document& drawingDocument);
    // Что стало с размерами при последнем обновлении видов: сколько переехало за моделью
    // и сколько потеряло привязку (такие размеры получают имя «Размер: привязка потеряна»).
    const drawing::ModelViewUpdate& lastViewUpdate() const { return lastViewUpdate_; }

    // Общий знак шероховатости в правом верхнем углу листа (ГОСТ 2.309-73, пп. 2.6, 2.7): знак примерно
    // в 1,5 раза крупнее знаков на изображении; others — рядом знак в скобках («остальные поверхности»).
    // Прежний общий знак заменяется. Пустое значение и вид Any без others убирают знак.
    core::Result<void> setSheetRoughness(document::Document& drawingDocument, const std::string& parameter,
                                         drawing::RoughnessSign::Kind kind, bool others);

    // Технические требования над основной надписью (ГОСТ 2.316-2008, пп. 4.10, 4.13, 4.14): пункты
    // нумеруются подряд, каждый с новой строки, колонка не шире 185 мм, заголовок не пишется.
    // Прежние требования заменяются; пустой список убирает их.
    core::Result<void> setTechnicalRequirements(document::Document& drawingDocument,
                                                const std::vector<std::string>& items);
    std::vector<std::string> technicalRequirements(const document::Document& drawingDocument) const;

    // Замкнутые контуры из выбранных линий и окружностей чертежа — заготовка эскиза.
    // Контур внутри другого контура считается отверстием.
    static core::Result<std::vector<modeling::SketchShape>> contourFromDrawing(
        const document::Document& drawingDocument, const std::vector<core::Uuid>& ids);

    // Значение формулы по параметрам детали («длина / 2 + 5»).
    core::Result<double> evaluateWith(const document::Document& part, const std::string& expression) const;
    // Связь «параметры детали → ограничения на чертеже → эскиз детали». Вызывается после изменения
    // параметров: формулы ограничений, которые ссылаются на эту деталь, пересчитываются, контуры на
    // чертежах подгоняются, а эскизы детали, снятые с этих контуров, обновляются.
    // Возвращает число обновлённых эскизов. Деталь после этого нужно перестроить.
    core::Result<int> syncSketches(document::Document& part);

    // Состав изделия сборки в файл CSV (разделитель «;», кодировка UTF-8 с меткой — открывается в Excel).
    core::Result<void> exportBillOfMaterialsCsv(document::Document& assemblyDocument,
                                                const std::filesystem::path& path);

    struct DemoModel {
        document::Document* support = nullptr;   // деталь «Опора»
        document::Document* pin = nullptr;       // деталь «Палец»
        document::Document* assembly = nullptr;  // сборка
        document::Document* drawing = nullptr;   // чертёж опоры с видами по модели
    };
    // Создаёт в проекте пример: две параметрические детали, сборку и чертёж с видами.
    core::Result<DemoModel> createDemoModel();

private:
    AppContext() = default;

    class PartSource final : public assembly::IPartSource {
    public:
        explicit PartSource(AppContext& context) : context_(context) {}
        core::Result<geometry::ShapePtr> bodyOf(const core::Uuid& documentId) override;
        core::Result<assembly::PartInfo> infoOf(const core::Uuid& documentId) override;

    private:
        AppContext& context_;
        std::vector<core::Uuid> building_;  // сборки, тела которых строятся прямо сейчас (защита от вложения в себя)
    };


    std::unique_ptr<storage::ProjectStorage> storage_;
    std::unique_ptr<document::DocumentService> documents_;
    std::unique_ptr<standards::StandardsService> standards_;
    drawing::DrawingService drawing_;
    drawing::ModelViewUpdate lastViewUpdate_;
    core::Result<std::pair<geometry::ShapePtr, geometry::ShapePtr>> sectionBodies(document::Document& source);
    struct ViewProjection {
        geometry::ProjectedView projected;
        std::optional<drawing::SectionMark> mark;
    };
    core::Result<ViewProjection> projectView(const document::Document& drawingDocument, const drawing::ModelView& view,
                                             double at);
    std::unique_ptr<schematic::SchematicService> schematic_;
    sketcher::SketchService sketcher_;
    std::unique_ptr<libraries::FastenerLibrary> fasteners_;
    std::unique_ptr<libraries::MaterialLibrary> materials_;
    std::unique_ptr<piping::PipeLibrary> pipes_;
    std::unique_ptr<libraries::ProfileLibrary> profiles_;
    std::filesystem::path userDirectory_;
    std::unique_ptr<PluginManager> plugins_;  // последним: уничтожается первым, пока остальное ещё живо
    std::unique_ptr<geometry::IGeometryKernel> kernel_;
    std::unique_ptr<modeling::ModelingService> modeling_;
    std::unique_ptr<PartSource> partSource_;
    std::unique_ptr<assembly::AssemblyService> assembly_;
};

}  // namespace cosmocad::app
