// Sketcher — геометрические и размерные ограничения для линий, окружностей и дуг чертежа
// и решатель, который подгоняет геометрию под эти ограничения.
//
// Ограничение — объект документа: вид, ссылки на элементы и, для размерных, значение.
// Решатель численный (метод Левенберга — Марквардта): начинает с нынешнего положения элементов
// и ищет ближайшее положение, в котором все ограничения выполнены. Поэтому фигура не «прыгает»,
// а результат при одинаковых исходных данных одинаков.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/core/Uuid.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/geometry/Geometry2D.h"

namespace cosmocad::sketcher {

inline constexpr int kSketcherApiVersion = 1;

enum class ConstraintKind {
    Fixed,          // элемент закреплён на месте
    Coincident,     // две точки совпадают
    Horizontal,     // отрезок горизонтален
    Vertical,       // отрезок вертикален
    Parallel,       // два отрезка параллельны
    Perpendicular,  // два отрезка перпендикулярны
    EqualLength,    // два отрезка равной длины
    EqualRadius,    // две окружности или дуги равного радиуса
    Concentric,     // общий центр двух окружностей или дуг
    Tangent,        // отрезок касается окружности или дуги; или две окружности касаются друг друга
    PointOnLine,    // точка лежит на прямой отрезка
    Length,         // длина отрезка = value
    Radius,         // радиус окружности или дуги = value
    Distance,       // расстояние между двумя точками = value
    Angle,          // угол между двумя отрезками = value, градусы
    Symmetric       // две точки симметричны относительно прямой отрезка
};
const char* toString(ConstraintKind kind);

// Какая точка элемента: у отрезка — начало или конец, у окружности и дуги — центр.
enum class PointRole { Whole = -1, Start = 0, End = 1, Center = 2 };

struct Ref {
    core::Uuid entityId;
    PointRole point = PointRole::Whole;
};

// Ограничение. Имя для показа — в Object::name.
class Constraint final : public document::ObjectT<Constraint> {
public:
    static constexpr const char* kType = "Constraint";
    ConstraintKind kind = ConstraintKind::Fixed;
    std::vector<Ref> refs;
    double value = 0.0;
    // Значение формулой от параметров детали («длина / 2»). Сам модуль формул не считает: приложение
    // вычисляет формулу по параметрам документа parameterDocumentId и передаёт число через setValues.
    std::string expression;
    core::Uuid parameterDocumentId;

protected:
    void writeJson(nlohmann::json& json) const override;
    document::Result<void> readJson(const nlohmann::json& json) override;
};

void registerTypes(document::ObjectFactory& factory);

struct ConstraintParams {
    ConstraintKind kind = ConstraintKind::Fixed;
    std::vector<Ref> refs;
    double value = 0.0;
    std::string expression{};            // формула, по которой получено value; пусто — просто число
    core::Uuid parameterDocumentId{};    // деталь, чьи параметры использует формула
};

// Состояние эскиза: недоопределён (остались свободные движения), полностью определён, переопределён
// (есть лишние или противоречивые ограничения).
enum class SketchState { Empty, UnderDefined, FullyDefined, OverDefined };

struct SolveReport {
    bool solved = false;      // решение, при котором выполнены все ограничения, существует и найдено
    bool satisfied = false;   // ограничения выполнены уже сейчас, без подгонки (до пересчёта)
    int iterations = 0;
    int variables = 0;        // сколько чисел описывает элементы с ограничениями
    int freedom = 0;          // сколько независимых движений у них осталось (степени свободы)
    int redundant = 0;        // сколько условий лишние: повторяют то, что уже задано другими
    int movedEntities = 0;    // сколько элементов решатель сдвинул
    double residual = 0.0;    // наибольшее оставшееся отклонение, мм
    std::vector<core::Uuid> unsatisfied;  // ограничения, которые выполнить не удалось

    SketchState state() const {
        if (variables == 0) return SketchState::Empty;
        if (!solved || redundant > 0) return SketchState::OverDefined;
        return freedom == 0 ? SketchState::FullyDefined : SketchState::UnderDefined;
    }
};

class SketchService {
public:
    // Добавляет ограничение и сразу подгоняет геометрию. Если ограничение противоречит имеющимся,
    // ничего не меняется, а в ошибке названы ограничения, с которыми оно не уживается.
    core::Result<core::Uuid> addConstraint(document::Document& document, const ConstraintParams& params);
    core::Result<void> removeConstraint(document::Document& document, const core::Uuid& constraintId);
    // Меняет значение размерного ограничения и подгоняет геометрию; при противоречии — отказ без изменений.
    core::Result<void> setValue(document::Document& document, const core::Uuid& constraintId, double value);
    // То же для нескольких ограничений сразу: одна подгонка, один шаг отмены. Возвращает число сдвинутых элементов.
    // keepFormulas — не стирать формулы (значения пересчитаны по тем же формулам после изменения параметров).
    core::Result<int> setValues(document::Document& document, const std::map<core::Uuid, double>& values,
                                bool keepFormulas = false);
    // Удаляет ограничения, чьи элементы удалены. Возвращает число удалённых.
    core::Result<int> removeOrphans(document::Document& document);

    // Подгоняет геометрию под все ограничения документа (после правки элементов обычными инструментами).
    // Изменения элементов — одна транзакция.
    core::Result<SolveReport> solve(document::Document& document);
    // То же без изменения документа: выполнены ли ограничения и сколько степеней свободы осталось.
    core::Result<SolveReport> analyze(const document::Document& document) const;

    // Ограничения, в которых участвует элемент.
    std::vector<const Constraint*> constraintsOf(const document::Document& document, const core::Uuid& entityId) const;
};

}  // namespace cosmocad::sketcher
