// Адаптер OCCT. Переводит типы CosmoCad в типы OCCT и обратно и ведёт
// устойчивые имена граней (см. описание в IGeometryKernel.h).
#include "cosmocad/geometry/OcctKernel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <NCollection_DataMap.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

namespace cosmocad::geometry {

using core::Error;
using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

using NameMap = NCollection_DataMap<TopoDS_Shape, std::string, TopTools_ShapeMapHasher>;

class OcctShape final : public IShape {
public:
    TopoDS_Shape shape;
    NameMap names;                             // грань → имя
    std::map<std::string, TopoDS_Face> faces;  // имя → грань

    void setName(const TopoDS_Shape& face, const std::string& name) {
        names.Bind(face, name);
        faces[name] = TopoDS::Face(face);
    }
};

std::shared_ptr<const OcctShape> cast(const ShapePtr& shape) {
    return std::dynamic_pointer_cast<const OcctShape>(shape);
}

Error failure(const char* what, const Standard_Failure& e) {
    const char* text = e.GetMessageString();
    return makeError(ErrorCode::Internal,
                     std::string(what) + ": геометрическое ядро сообщило об ошибке" +
                         (text && *text ? std::string(" (") + text + ")" : std::string()));
}

bool cancelled(const CancelFlag* cancel) { return cancel && cancel->load(); }

gp_Ax3 toAx3(const Plane& plane) {
    return gp_Ax3(gp_Pnt(plane.origin.x, plane.origin.y, plane.origin.z),
                  gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z),
                  gp_Dir(plane.xDirection.x, plane.xDirection.y, plane.xDirection.z));
}

gp_Pnt to3d(const gp_Ax3& ax, Point2 p) {
    return ax.Location().Translated(gp_Vec(ax.XDirection()) * p.x + gp_Vec(ax.YDirection()) * p.y);
}

struct FaceKey {
    double x, y, z, area;
};

FaceKey faceKey(const TopoDS_Shape& face) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const gp_Pnt c = props.CentreOfMass();
    const auto round6 = [](double v) { return std::round(v * 1e6) / 1e6; };
    return FaceKey{round6(c.X()), round6(c.Y()), round6(c.Z()), round6(props.Mass())};
}

bool keyLess(const FaceKey& a, const FaceKey& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.area < b.area;
}

void sortFaces(std::vector<TopoDS_Shape>& faces) {
    std::vector<std::pair<FaceKey, TopoDS_Shape>> keyed;
    keyed.reserve(faces.size());
    for (const auto& face : faces) keyed.emplace_back(faceKey(face), face);
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) { return keyLess(a.first, b.first); });
    for (std::size_t i = 0; i < faces.size(); ++i) faces[i] = keyed[i].second;
}

// Даёт имена: первая грань получает «имя», остальные — «имя#2», «имя#3»…
void assignNames(OcctShape& out, const std::string& base, std::vector<TopoDS_Shape> faces) {
    faces.erase(std::remove_if(faces.begin(), faces.end(),
                               [&](const TopoDS_Shape& f) { return out.names.IsBound(f); }),
                faces.end());
    sortFaces(faces);
    // Имя может быть уже занято (два тела с одинаковыми именами объединены) — берём следующее свободное.
    std::size_t number = 1;
    for (const auto& face : faces) {
        std::string name;
        do {
            name = number == 1 ? base : base + "#" + std::to_string(number);
            ++number;
        } while (out.faces.count(name) > 0);
        out.setName(face, name);
    }
}

// Переносит имена граней исходных тел на результат операции.
void propagateNames(BRepBuilderAPI_MakeShape& builder, const std::vector<const OcctShape*>& inputs,
                    OcctShape& out) {
    TopTools_IndexedMapOfShape resultFaces;
    TopExp::MapShapes(out.shape, TopAbs_FACE, resultFaces);

    // Порядок обхода — по имени, чтобы результат не зависел от внутреннего порядка таблиц.
    for (const OcctShape* input : inputs) {
        for (const auto& [name, face] : input->faces) {
            std::vector<TopoDS_Shape> candidates;
            const TopTools_ListOfShape& modified = builder.Modified(face);
            if (modified.IsEmpty()) {
                if (!builder.IsDeleted(face) && resultFaces.Contains(face)) candidates.push_back(face);
            } else {
                for (const TopoDS_Shape& m : modified)
                    if (resultFaces.Contains(m)) candidates.push_back(m);
            }
            if (!candidates.empty()) assignNames(out, name, std::move(candidates));
        }
    }
}

// Грани, оставшиеся без имени, получают «<метка>/new/<k>» в порядке координат.
void nameRemaining(OcctShape& out, const std::string& tag) {
    std::vector<TopoDS_Shape> rest;
    TopTools_IndexedMapOfShape all;
    TopExp::MapShapes(out.shape, TopAbs_FACE, all);
    for (int i = 1; i <= all.Extent(); ++i)
        if (!out.names.IsBound(all(i))) rest.push_back(all(i));
    sortFaces(rest);
    for (std::size_t i = 0; i < rest.size(); ++i) out.setName(rest[i], tag + "/new/" + std::to_string(i + 1));
}

// ---- Контур эскиза → грань OCCT ----

double signedArea(const std::vector<ProfileSegment>& loop) {
    if (loop.size() == 1 && loop[0].kind == ProfileSegment::Kind::Circle) return 1.0;  // строится против часовой
    double area = 0.0;
    const auto add = [&](Point2 a, Point2 b) { area += cross(a, b) / 2.0; };
    for (const auto& s : loop) {
        if (s.kind == ProfileSegment::Kind::Arc) {
            const double r = distance(s.center, s.start);
            double a0 = std::atan2(s.start.y - s.center.y, s.start.x - s.center.x);
            double a1 = std::atan2(s.end.y - s.center.y, s.end.x - s.center.x);
            double sweep = s.clockwise ? normalizeAngle(a0 - a1) : normalizeAngle(a1 - a0);
            const double mid = s.clockwise ? a0 - sweep / 2.0 : a0 + sweep / 2.0;
            const Point2 m = pointOnCircle(s.center, r, mid);
            add(s.start, m);
            add(m, s.end);
        } else {
            add(s.start, s.end);
        }
    }
    return area;
}

struct BuiltWire {
    TopoDS_Wire wire;
    std::vector<std::pair<TopoDS_Edge, std::uint64_t>> edges;
};

Result<BuiltWire> buildWire(const std::vector<ProfileSegment>& loop, const gp_Ax3& ax, bool wantCounterClockwise) {
    if (loop.empty()) return makeError(ErrorCode::InvalidArgument, "контур эскиза пуст");
    BuiltWire out;
    BRepBuilderAPI_MakeWire wire;
    const gp_Ax2 ax2(ax.Location(), ax.Direction(), ax.XDirection());
    for (const auto& s : loop) {
        TopoDS_Edge edge;
        if (s.kind == ProfileSegment::Kind::Line) {
            if (almostEqual(s.start, s.end))
                return makeError(ErrorCode::InvalidArgument, "в контуре эскиза есть отрезок нулевой длины");
            edge = BRepBuilderAPI_MakeEdge(to3d(ax, s.start), to3d(ax, s.end));
        } else {
            const double r = distance(s.center, s.start);
            if (r < kLinearTolerance)
                return makeError(ErrorCode::InvalidArgument, "в контуре эскиза есть окружность нулевого радиуса");
            gp_Ax2 circleAx = ax2;
            circleAx.SetLocation(to3d(ax, s.center));
            const gp_Circ circle(circleAx, r);
            if (s.kind == ProfileSegment::Kind::Circle) {
                edge = BRepBuilderAPI_MakeEdge(circle);
            } else {
                GC_MakeArcOfCircle arc(circle, to3d(ax, s.start), to3d(ax, s.end), !s.clockwise);
                if (!arc.IsDone()) return makeError(ErrorCode::InvalidArgument, "не удалось построить дугу контура");
                edge = BRepBuilderAPI_MakeEdge(arc.Value());
            }
        }
        wire.Add(edge);
        if (!wire.IsDone())
            return makeError(ErrorCode::InvalidArgument,
                             "контур эскиза не замкнут или его элементы не соединены концами");
        // MakeWire при стыковке концов заменяет ребро копией — берём то, что реально вошло в контур.
        out.edges.emplace_back(wire.Edge(), s.sourceTag);
    }
    out.wire = wire.Wire();
    if (!out.wire.Closed() && !BRep_Tool::IsClosed(out.wire))
        return makeError(ErrorCode::InvalidArgument, "контур эскиза не замкнут");
    const bool isCounterClockwise = signedArea(loop) > 0.0;
    if (isCounterClockwise != wantCounterClockwise) out.wire.Reverse();
    return out;
}

struct BuiltFace {
    TopoDS_Face face;
    std::vector<std::pair<TopoDS_Edge, std::uint64_t>> edges;
};

Result<BuiltFace> buildFace(const Profile2D& profile, const Plane& plane) {
    const gp_Ax3 ax = toAx3(plane);
    auto outer = buildWire(profile.outer, ax, true);
    if (!outer) return outer.error();
    BuiltFace out;
    out.edges = outer.value().edges;
    BRepBuilderAPI_MakeFace face(gp_Pln(ax), outer.value().wire, true);
    for (const auto& hole : profile.holes) {
        auto wire = buildWire(hole, ax, false);
        if (!wire) return wire.error();
        face.Add(wire.value().wire);
        out.edges.insert(out.edges.end(), wire.value().edges.begin(), wire.value().edges.end());
    }
    if (!face.IsDone()) return makeError(ErrorCode::InvalidArgument, "не удалось построить плоскую фигуру по контуру");
    out.face = face.Face();
    if (!BRepCheck_Analyzer(out.face).IsValid())
        return makeError(ErrorCode::InvalidArgument,
                         "контур эскиза пересекает сам себя или отверстие выходит за контур");
    return out;
}

// Имена граней тела, полученного движением плоской фигуры (выдавливание, вращение).
template <class Sweep>
void nameSweep(Sweep& sweep, const BuiltFace& base, const std::string& tag, OcctShape& out) {
    TopTools_IndexedMapOfShape resultFaces;
    TopExp::MapShapes(out.shape, TopAbs_FACE, resultFaces);
    for (const auto& [edge, sourceTag] : base.edges) {
        std::vector<TopoDS_Shape> generated;
        for (const TopoDS_Shape& g : sweep.Generated(edge))
            if (g.ShapeType() == TopAbs_FACE && resultFaces.Contains(g)) generated.push_back(g);
        if (!generated.empty()) assignNames(out, tag + "/side/" + std::to_string(sourceTag), std::move(generated));
    }
    const TopoDS_Shape first = sweep.FirstShape();
    const TopoDS_Shape last = sweep.LastShape();
    if (!first.IsNull() && first.ShapeType() == TopAbs_FACE && resultFaces.Contains(first))
        assignNames(out, tag + "/start", {first});
    if (!last.IsNull() && last.ShapeType() == TopAbs_FACE && resultFaces.Contains(last))
        assignNames(out, tag + "/end", {last});
    nameRemaining(out, tag);
}

double volumeOf(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

Result<void> checkSolid(const TopoDS_Shape& shape, const char* what) {
    if (shape.IsNull()) return makeError(ErrorCode::Internal, std::string(what) + ": тело не получилось");
    if (!BRepCheck_Analyzer(shape).IsValid())
        return makeError(ErrorCode::Internal, std::string(what) + ": получилось некорректное тело");
    return {};
}

std::vector<TopoDS_Edge> findEdges(const OcctShape& shape, const EdgeRef& ref) {
    std::vector<TopoDS_Edge> found;
    const auto a = shape.faces.find(ref.faceA);
    const auto b = shape.faces.find(ref.faceB);
    if (a == shape.faces.end() || b == shape.faces.end()) return found;
    TopTools_IndexedMapOfShape edgesA, edgesB;
    TopExp::MapShapes(a->second, TopAbs_EDGE, edgesA);
    TopExp::MapShapes(b->second, TopAbs_EDGE, edgesB);
    for (int i = 1; i <= edgesA.Extent(); ++i)
        if (edgesB.Contains(edgesA(i)) && !BRep_Tool::Degenerated(TopoDS::Edge(edgesA(i))))
            found.push_back(TopoDS::Edge(edgesA(i)));
    return found;
}

Point3 toPoint(const gp_Pnt& p) { return Point3{p.X(), p.Y(), p.Z()}; }

// Перевод кривой проекции (лежит в плоскости XY) в линию вида.
void appendProjected(const TopoDS_Shape& compound, bool hidden, ProjectedView& view) {
    if (compound.IsNull()) return;
    for (TopExp_Explorer it(compound, TopAbs_EDGE); it.More(); it.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(it.Current());
        BRepAdaptor_Curve curve(edge);
        const double first = curve.FirstParameter(), last = curve.LastParameter();
        const gp_Pnt p0 = curve.Value(first), p1 = curve.Value(last);
        ProjectedCurve out;
        out.hidden = hidden;
        if (curve.GetType() == GeomAbs_Line) {
            out.kind = ProjectedCurve::Kind::Line;
            out.a = Point2{p0.X(), p0.Y()};
            out.b = Point2{p1.X(), p1.Y()};
            if (distance(out.a, out.b) > 1e-7) view.curves.push_back(out);
        } else if (curve.GetType() == GeomAbs_Circle) {
            const gp_Circ circle = curve.Circle();
            out.center = Point2{circle.Location().X(), circle.Location().Y()};
            out.radius = circle.Radius();
            if (std::abs(std::abs(last - first) - 2.0 * std::numbers::pi) < 1e-7) {
                out.kind = ProjectedCurve::Kind::Circle;
            } else {
                out.kind = ProjectedCurve::Kind::Arc;
                const gp_Pnt pm = curve.Value((first + last) / 2.0);
                const auto angle = [&](const gp_Pnt& p) {
                    return normalizeAngle(std::atan2(p.Y() - out.center.y, p.X() - out.center.x));
                };
                double a0 = angle(p0), a1 = angle(p1);
                if (!angleOnArc(angle(pm), a0, a1)) std::swap(a0, a1);
                out.startAngle = a0;
                out.endAngle = a1;
            }
            view.curves.push_back(out);
        } else {
            GCPnts_QuasiUniformDeflection points(curve, 0.01);
            if (!points.IsDone()) continue;
            out.kind = ProjectedCurve::Kind::Line;
            for (int i = 1; i < points.NbPoints(); ++i) {
                const gp_Pnt a = points.Value(i), b = points.Value(i + 1);
                out.a = Point2{a.X(), a.Y()};
                out.b = Point2{b.X(), b.Y()};
                if (distance(out.a, out.b) > 1e-7) view.curves.push_back(out);
            }
        }
    }
}

class OcctKernel final : public IGeometryKernel {
public:
    int apiVersion() const override { return kGeometryKernelApiVersion; }

    Result<ShapePtr> extrude(const Profile2D& profile, const Plane& plane, double distance, const std::string& tag,
                             const CancelFlag* cancel) override {
        if (cancelled(cancel)) return makeError(ErrorCode::Cancelled, "операция отменена");
        if (std::abs(distance) < kLinearTolerance)
            return makeError(ErrorCode::InvalidArgument, "расстояние выдавливания равно нулю");
        try {
            auto base = buildFace(profile, plane);
            if (!base) return base.error();
            const gp_Dir n(plane.normal.x, plane.normal.y, plane.normal.z);
            BRepPrimAPI_MakePrism prism(base.value().face, gp_Vec(n) * distance);
            prism.Build();
            if (!prism.IsDone()) return makeError(ErrorCode::Internal, "выдавливание не выполнено");
            auto out = std::make_shared<OcctShape>();
            out->shape = prism.Shape();
            if (volumeOf(out->shape) < 0.0) out->shape.Reverse();
            if (auto ok = checkSolid(out->shape, "выдавливание"); !ok) return ok.error();
            nameSweep(prism, base.value(), tag, *out);
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("выдавливание", e);
        }
    }

    Result<ShapePtr> revolve(const Profile2D& profile, const Plane& plane, const Point3& axisPoint,
                             const Vector3& axisDirection, double angleDegrees, const std::string& tag,
                             const CancelFlag* cancel) override {
        if (cancelled(cancel)) return makeError(ErrorCode::Cancelled, "операция отменена");
        if (angleDegrees <= 0.0 || angleDegrees > 360.0 + 1e-9)
            return makeError(ErrorCode::InvalidArgument, "угол вращения должен быть больше 0 и не больше 360 градусов");
        try {
            auto base = buildFace(profile, plane);
            if (!base) return base.error();
            const gp_Ax1 axis(gp_Pnt(axisPoint.x, axisPoint.y, axisPoint.z),
                              gp_Dir(axisDirection.x, axisDirection.y, axisDirection.z));
            const bool full = angleDegrees >= 360.0 - 1e-9;
            BRepPrimAPI_MakeRevol revol = full ? BRepPrimAPI_MakeRevol(base.value().face, axis)
                                               : BRepPrimAPI_MakeRevol(base.value().face, axis,
                                                                       angleDegrees * std::numbers::pi / 180.0);
            revol.Build();
            if (!revol.IsDone()) return makeError(ErrorCode::Internal, "вращение не выполнено");
            auto out = std::make_shared<OcctShape>();
            out->shape = revol.Shape();
            if (volumeOf(out->shape) < 0.0) out->shape.Reverse();
            if (auto ok = checkSolid(out->shape, "вращение"); !ok)
                return makeError(ErrorCode::InvalidArgument,
                                 "вращение: контур пересекает ось вращения или получилось некорректное тело");
            nameSweep(revol, base.value(), tag, *out);
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("вращение", e);
        }
    }

    Result<ShapePtr> sweep(const Profile2D& profile, const Plane& plane, const std::vector<Point3>& path, double bendRadius,
                           const std::string& tag, const CancelFlag* cancel) override {
        if (cancelled(cancel)) return makeError(ErrorCode::Cancelled, "операция отменена");
        if (path.size() < 2) return makeError(ErrorCode::InvalidArgument, "траектория протяжки задаётся не меньше чем двумя точками");
        if (path.size() > 2 && !(bendRadius > 0.0))
            return makeError(ErrorCode::InvalidArgument, "у траектории с изломами задайте радиус гиба больше нуля");
        try {
            auto base = buildFace(profile, plane);
            if (!base) return base.error();
            std::vector<gp_Pnt> points;
            for (const auto& p : path) points.emplace_back(p.x, p.y, p.z);
            for (std::size_t i = 0; i + 1 < points.size(); ++i)
                if (points[i].Distance(points[i + 1]) < kLinearTolerance)
                    return makeError(ErrorCode::InvalidArgument, "в траектории протяжки две точки совпадают");
            const gp_Dir normal(plane.normal.x, plane.normal.y, plane.normal.z);
            if (std::abs(gp_Vec(points[0], points[1]).Normalized().Dot(gp_Vec(normal))) < 0.1)
                return makeError(ErrorCode::InvalidArgument, "траектория протяжки должна уходить из плоскости контура, а не лежать в ней");

            // Траектория: прямые участки и дуги радиуса bendRadius в изломах.
            BRepBuilderAPI_MakeWire spine;
            gp_Pnt from = points[0];
            for (std::size_t i = 1; i < points.size(); ++i) {
                gp_Pnt to = points[i];
                if (i + 1 < points.size()) {
                    const gp_Vec back(points[i], points[i - 1]), ahead(points[i], points[i + 1]);
                    const double angle = back.Angle(ahead);  // угол между участками в изломе
                    if (angle < 1e-6) return makeError(ErrorCode::InvalidArgument, "траектория протяжки поворачивает назад");
                    if (std::abs(angle - std::numbers::pi) > 1e-6) {
                        const double cut = bendRadius / std::tan(angle / 2.0);
                        const double room = std::min(i == 1 ? back.Magnitude() : back.Magnitude() / 2.0,
                                                     i + 2 == points.size() ? ahead.Magnitude() : ahead.Magnitude() / 2.0);
                        if (cut > room + 1e-9)
                            return makeError(ErrorCode::InvalidArgument, "радиус гиба слишком велик для участков траектории");
                        const gp_Pnt start = points[i].Translated(back.Normalized() * cut);
                        const gp_Pnt end = points[i].Translated(ahead.Normalized() * cut);
                        const gp_Vec bisector = (back.Normalized() + ahead.Normalized()).Normalized();
                        const gp_Pnt center = points[i].Translated(bisector * (bendRadius / std::sin(angle / 2.0)));
                        const gp_Pnt middle = center.Translated(gp_Vec(center, points[i]).Normalized() * bendRadius);
                        if (from.Distance(start) > kLinearTolerance) spine.Add(BRepBuilderAPI_MakeEdge(from, start).Edge());
                        GC_MakeArcOfCircle arc(start, middle, end);
                        if (!arc.IsDone()) return makeError(ErrorCode::Internal, "не удалось построить гиб траектории");
                        spine.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
                        from = end;
                        continue;
                    }
                }
                // Последняя точка или точка на прямой: участок дотягивается до следующего излома или до конца.
                if (i + 1 == points.size()) {
                    if (from.Distance(to) > kLinearTolerance) spine.Add(BRepBuilderAPI_MakeEdge(from, to).Edge());
                    from = to;
                }
            }
            if (!spine.IsDone()) return makeError(ErrorCode::Internal, "не удалось построить траекторию протяжки");
            BRepOffsetAPI_MakePipe pipe(spine.Wire(), base.value().face);
            pipe.Build();
            if (!pipe.IsDone()) return makeError(ErrorCode::Internal, "протяжка не выполнена");
            auto out = std::make_shared<OcctShape>();
            out->shape = pipe.Shape();
            if (volumeOf(out->shape) < 0.0) out->shape.Reverse();
            if (auto ok = checkSolid(out->shape, "протяжка"); !ok) return ok.error();
            nameRemaining(*out, tag);
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("протяжка", e);
        }
    }

    Result<ShapePtr> boolean(BooleanKind kind, const ShapePtr& a, const ShapePtr& b,
                             const CancelFlag* cancel) override {
        if (cancelled(cancel)) return makeError(ErrorCode::Cancelled, "операция отменена");
        const auto sa = cast(a), sb = cast(b);
        if (!sa || !sb) return makeError(ErrorCode::InvalidArgument, "булева операция: нет одного из тел");
        try {
            std::unique_ptr<BRepAlgoAPI_BooleanOperation> op;
            switch (kind) {
                case BooleanKind::Union: op = std::make_unique<BRepAlgoAPI_Fuse>(sa->shape, sb->shape); break;
                case BooleanKind::Subtract: op = std::make_unique<BRepAlgoAPI_Cut>(sa->shape, sb->shape); break;
                case BooleanKind::Intersect: op = std::make_unique<BRepAlgoAPI_Common>(sa->shape, sb->shape); break;
            }
            if (!op->IsDone() || op->HasErrors()) return makeError(ErrorCode::Internal, "булева операция не выполнена");
            auto out = std::make_shared<OcctShape>();
            out->shape = op->Shape();
            bool hasSolid = false;
            for (TopExp_Explorer it(out->shape, TopAbs_SOLID); it.More(); it.Next()) hasSolid = true;
            if (!hasSolid) return makeError(ErrorCode::InvalidState, "после операции от тела ничего не осталось");
            if (auto ok = checkSolid(out->shape, "булева операция"); !ok) return ok.error();
            propagateNames(*op, {sa.get(), sb.get()}, *out);
            nameRemaining(*out, "boolean");
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("булева операция", e);
        }
    }

    Result<ShapePtr> fillet(const ShapePtr& shape, const std::vector<EdgeRef>& edges, double radius,
                            const std::string& tag) override {
        return blend(shape, edges, radius, tag, true);
    }

    Result<ShapePtr> chamfer(const ShapePtr& shape, const std::vector<EdgeRef>& edges, double distance,
                             const std::string& tag) override {
        return blend(shape, edges, distance, tag, false);
    }

    Result<ShapePtr> transformed(const ShapePtr& shape, const Placement& placement) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "перемещение: нет тела");
        try {
            gp_Trsf trsf;
            const auto& m = placement.m;
            trsf.SetValues(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            BRepBuilderAPI_Transform op(s->shape, trsf, false);
            auto out = std::make_shared<OcctShape>();
            out->shape = op.Shape();
            for (const auto& [name, face] : s->faces) {
                const TopoDS_Shape moved = op.ModifiedShape(face);
                if (!moved.IsNull()) out->setName(moved, name);
            }
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("перемещение", e);
        }
    }

    Result<ShapePtr> mirrored(const ShapePtr& shape, const Point3& planePoint, const Vector3& planeNormal,
                              const std::string& tag) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "зеркальное отражение: нет тела");
        try {
            const Vector3 n = normalized(planeNormal);
            gp_Trsf trsf;
            trsf.SetMirror(gp_Ax2(gp_Pnt(planePoint.x, planePoint.y, planePoint.z), gp_Dir(n.x, n.y, n.z)));
            BRepBuilderAPI_Transform op(s->shape, trsf, true);
            auto out = std::make_shared<OcctShape>();
            out->shape = op.Shape();
            for (const auto& [name, face] : s->faces) {
                const TopoDS_Shape moved = op.ModifiedShape(face);
                if (!moved.IsNull()) out->setName(moved, tag + "/mirror/" + name);
            }
            if (auto ok = checkSolid(out->shape, "зеркальное отражение"); !ok) return ok.error();
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("зеркальное отражение", e);
        }
    }

    Result<ShapePtr> combine(const std::vector<ShapePtr>& shapes) override {
        if (shapes.empty()) return makeError(ErrorCode::InvalidArgument, "нет тел");
        auto out = std::make_shared<OcctShape>();
        TopoDS_Compound compound;
        TopoDS_Builder builder;
        builder.MakeCompound(compound);
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const auto s = cast(shapes[i]);
            if (!s) return makeError(ErrorCode::InvalidArgument, "нет одного из тел");
            builder.Add(compound, s->shape);
            for (const auto& [name, face] : s->faces)
                if (!out->names.IsBound(face)) out->setName(face, std::to_string(i + 1) + ":" + name);
        }
        out->shape = compound;
        return ShapePtr(out);
    }

    std::vector<std::string> faceNames(const ShapePtr& shape) const override {
        std::vector<std::string> names;
        if (const auto s = cast(shape))
            for (const auto& [name, face] : s->faces) names.push_back(name);
        return names;
    }

    Result<FaceInfo> faceInfo(const ShapePtr& shape, const std::string& faceName) const override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "нет тела");
        const auto it = s->faces.find(faceName);
        if (it == s->faces.end()) return makeError(ErrorCode::NotFound, "грань «" + faceName + "» не найдена");
        try {
            // Берём грань так, как она стоит в теле: от этого зависит, куда смотрит нормаль.
            TopoDS_Face face = it->second;
            for (TopExp_Explorer ex(s->shape, TopAbs_FACE); ex.More(); ex.Next())
                if (ex.Current().IsSame(it->second)) {
                    face = TopoDS::Face(ex.Current());
                    break;
                }
            FaceInfo info;
            GProp_GProps props;
            BRepGProp::SurfaceProperties(face, props);
            info.area = props.Mass();
            info.centroid = toPoint(props.CentreOfMass());
            BRepAdaptor_Surface surface(face);
            if (surface.GetType() == GeomAbs_Plane) {
                info.kind = FaceInfo::Kind::Plane;
                const gp_Pln plane = surface.Plane();
                gp_Dir n = plane.Axis().Direction();
                if (!plane.Direct()) n.Reverse();
                if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
                info.point = info.centroid;
                info.direction = Vector3{n.X(), n.Y(), n.Z()};
            } else if (surface.GetType() == GeomAbs_Cylinder) {
                info.kind = FaceInfo::Kind::Cylinder;
                const gp_Cylinder cylinder = surface.Cylinder();
                info.point = toPoint(cylinder.Location());
                const gp_Dir d = cylinder.Axis().Direction();
                info.direction = Vector3{d.X(), d.Y(), d.Z()};
                info.radius = cylinder.Radius();
            }
            return info;
        } catch (const Standard_Failure& e) {
            return failure("сведения о грани", e);
        }
    }

    bool hasEdge(const ShapePtr& shape, const EdgeRef& edge) const override {
        const auto s = cast(shape);
        return s && !findEdges(*s, edge).empty();
    }

    Result<void> validate(const ShapePtr& shape) override {
        const auto s = cast(shape);
        if (!s || s->shape.IsNull()) return makeError(ErrorCode::InvalidArgument, "нет тела");
        if (!BRepCheck_Analyzer(s->shape).IsValid()) return makeError(ErrorCode::InvalidState, "тело некорректно");
        return {};
    }

    Result<MassProperties> massProperties(const ShapePtr& shape) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "нет тела");
        try {
            MassProperties out;
            GProp_GProps volume, surface;
            BRepGProp::VolumeProperties(s->shape, volume);
            BRepGProp::SurfaceProperties(s->shape, surface);
            out.volume = volume.Mass();
            out.surfaceArea = surface.Mass();
            out.centerOfMass = toPoint(volume.CentreOfMass());
            return out;
        } catch (const Standard_Failure& e) {
            return failure("расчёт массы", e);
        }
    }

    Result<Mesh> tessellate(const ShapePtr& shape, double linearDeflection, const CancelFlag* cancel) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "нет тела");
        if (cancelled(cancel)) return makeError(ErrorCode::Cancelled, "операция отменена");
        try {
            // Сетка строится на копии ссылки: BRepMesh хранит треугольники внутри тела.
            BRepMesh_IncrementalMesh mesher(s->shape, linearDeflection, false, 0.3, false);
            Mesh mesh;
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(s->shape, TopAbs_FACE, faces);
            for (int f = 1; f <= faces.Extent(); ++f) {
                if (cancelled(cancel)) return makeError(ErrorCode::Cancelled, "операция отменена");
                const TopoDS_Face face = TopoDS::Face(faces(f));
                TopLoc_Location location;
                const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
                if (tri.IsNull()) continue;
                const gp_Trsf trsf = location.Transformation();
                const bool reversed = face.Orientation() == TopAbs_REVERSED;
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                const auto faceIndex = static_cast<std::uint32_t>(mesh.faceNames.size());
                mesh.faceNames.push_back(s->names.IsBound(face) ? s->names.Find(face) : std::string());
                BRepAdaptor_Surface surface(face);
                for (int i = 1; i <= tri->NbNodes(); ++i) {
                    mesh.vertices.push_back(toPoint(tri->Node(i).Transformed(trsf)));
                    Vector3 normal{0.0, 0.0, 0.0};
                    if (tri->HasUVNodes()) {
                        const gp_Pnt2d uv = tri->UVNode(i);
                        BRepLProp_SLProps props(surface, uv.X(), uv.Y(), 1, 1e-6);
                        if (props.IsNormalDefined()) {
                            gp_Dir n = props.Normal();
                            if (reversed) n.Reverse();
                            normal = Vector3{n.X(), n.Y(), n.Z()};
                        }
                    }
                    mesh.normals.push_back(normal);
                }
                for (int i = 1; i <= tri->NbTriangles(); ++i) {
                    int a, b, c;
                    tri->Triangle(i).Get(a, b, c);
                    if (reversed) std::swap(b, c);
                    mesh.triangles.push_back({base + static_cast<std::uint32_t>(a - 1),
                                              base + static_cast<std::uint32_t>(b - 1),
                                              base + static_cast<std::uint32_t>(c - 1)});
                    mesh.triangleFace.push_back(faceIndex);
                }
            }
            // Вершины без нормали (особые точки поверхности) получают нормаль своего треугольника.
            for (const auto& t : mesh.triangles) {
                const Point3& a = mesh.vertices[t[0]];
                const Point3& b = mesh.vertices[t[1]];
                const Point3& c = mesh.vertices[t[2]];
                const Vector3 n = normalized(crossProduct(Vector3{b.x - a.x, b.y - a.y, b.z - a.z},
                                                          Vector3{c.x - a.x, c.y - a.y, c.z - a.z}));
                for (auto index : t) {
                    Vector3& v = mesh.normals[index];
                    if (v.x == 0.0 && v.y == 0.0 && v.z == 0.0) v = n;
                }
            }
            TopTools_IndexedMapOfShape edges;
            TopExp::MapShapes(s->shape, TopAbs_EDGE, edges);
            for (int e = 1; e <= edges.Extent(); ++e) {
                const TopoDS_Edge edge = TopoDS::Edge(edges(e));
                if (BRep_Tool::Degenerated(edge)) continue;
                BRepAdaptor_Curve curve(edge);
                GCPnts_TangentialDeflection points(curve, 0.2, linearDeflection);
                std::vector<Point3> line;
                for (int i = 1; i <= points.NbPoints(); ++i) line.push_back(toPoint(points.Value(i)));
                if (line.size() >= 2) mesh.edges.push_back(std::move(line));
            }
            return mesh;
        } catch (const Standard_Failure& e) {
            return failure("построение сетки", e);
        }
    }

    Result<double> commonVolume(const ShapePtr& a, const ShapePtr& b) override {
        const auto sa = cast(a), sb = cast(b);
        if (!sa || !sb) return makeError(ErrorCode::InvalidArgument, "нет одного из тел");
        try {
            BRepAlgoAPI_Common op(sa->shape, sb->shape);
            if (!op.IsDone() || op.HasErrors()) return makeError(ErrorCode::Internal, "не удалось сравнить тела");
            bool hasSolid = false;
            for (TopExp_Explorer it(op.Shape(), TopAbs_SOLID); it.More(); it.Next()) hasSolid = true;
            return hasSolid ? std::abs(volumeOf(op.Shape())) : 0.0;
        } catch (const Standard_Failure& e) {
            return failure("проверка пересечений", e);
        }
    }

    Result<double> distance(const ShapePtr& a, const ShapePtr& b) override {
        const auto sa = cast(a), sb = cast(b);
        if (!sa || !sb) return makeError(ErrorCode::InvalidArgument, "нет одного из тел");
        try {
            BRepExtrema_DistShapeShape measure(sa->shape, sb->shape);
            if (!measure.IsDone()) return makeError(ErrorCode::Internal, "не удалось измерить расстояние между телами");
            return measure.Value();
        } catch (const Standard_Failure& e) {
            return failure("измерение зазора", e);
        }
    }

    Result<ProjectedView> project(const ShapePtr& shape, const Vector3& viewDirection, const Vector3& up,
                                  bool withHidden) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "нет тела");
        try {
            const Vector3 toEye = normalized(Vector3{-viewDirection.x, -viewDirection.y, -viewDirection.z});
            const Vector3 right = normalized(crossProduct(up, toEye));
            const gp_Ax2 ax(gp_Pnt(0, 0, 0), gp_Dir(toEye.x, toEye.y, toEye.z), gp_Dir(right.x, right.y, right.z));
            Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
            algo->Add(s->shape);
            algo->Projector(HLRAlgo_Projector(ax));
            algo->Update();
            algo->Hide();
            HLRBRep_HLRToShape extract(algo);
            ProjectedView view;
            const auto take = [&](TopoDS_Shape compound, bool hidden) {
                if (compound.IsNull()) return;
                BRepLib::BuildCurves3d(compound);
                appendProjected(compound, hidden, view);
            };
            take(extract.VCompound(), false);
            take(extract.OutLineVCompound(), false);
            if (withHidden) {
                take(extract.HCompound(), true);
                take(extract.OutLineHCompound(), true);
            }
            return view;
        } catch (const Standard_Failure& e) {
            return failure("построение вида", e);
        }
    }

    Result<ProjectedView> projectSection(const ShapePtr& shape, const Vector3& viewDirection, const Vector3& up,
                                         const Point3& planePoint, bool withHidden, const ShapePtr& whole,
                                         bool onlyCut) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "нет тела");
        const auto kept = cast(whole);
        try {
            const Vector3 toEye = normalized(Vector3{-viewDirection.x, -viewDirection.y, -viewDirection.z});
            const Vector3 right = normalized(crossProduct(up, toEye));
            const Vector3 upward = crossProduct(toEye, right);
            const gp_Pnt origin(planePoint.x, planePoint.y, planePoint.z);
            const gp_Pln plane(origin, gp_Dir(toEye.x, toEye.y, toEye.z));
            // Убирается всё, что лежит между плоскостью и наблюдателем.
            const TopoDS_Face cutter = BRepBuilderAPI_MakeFace(plane).Face();
            const gp_Pnt inFront = origin.Translated(gp_Vec(toEye.x, toEye.y, toEye.z));
            const TopoDS_Shape half = BRepPrimAPI_MakeHalfSpace(cutter, inFront).Solid();
            BRepAlgoAPI_Cut op(s->shape, half);
            if (!op.IsDone() || op.HasErrors()) return makeError(ErrorCode::Internal, "разрез не выполнен");
            auto rest = std::make_shared<OcctShape>();
            rest->shape = op.Shape();
            bool hasSolid = false;
            for (TopExp_Explorer it(rest->shape, TopAbs_SOLID); it.More(); it.Next()) hasSolid = true;
            if (!hasSolid)
                return makeError(ErrorCode::InvalidState, "секущая плоскость не проходит через тело: за ней ничего нет");

            ProjectedView view;
            TopoDS_Compound caps;
            TopoDS_Builder capBuilder;
            capBuilder.MakeCompound(caps);

            const auto flat = [&](const gp_Pnt& p) {
                return Point2{p.X() * right.x + p.Y() * right.y + p.Z() * right.z,
                              p.X() * upward.x + p.Y() * upward.y + p.Z() * upward.z};
            };
            int body = 0;
            for (TopExp_Explorer solid(rest->shape, TopAbs_SOLID); solid.More(); solid.Next(), ++body) {
                for (TopExp_Explorer it(solid.Current(), TopAbs_FACE); it.More(); it.Next()) {
                    const TopoDS_Face face = TopoDS::Face(it.Current());
                    BRepAdaptor_Surface surface(face);
                    if (surface.GetType() != GeomAbs_Plane) continue;
                    const gp_Pln found = surface.Plane();
                    if (!found.Axis().Direction().IsParallel(plane.Axis().Direction(), 1e-6)) continue;
                    if (plane.Distance(found.Location()) > 1e-6) continue;
                    // Грань исходного тела, случайно лежащая в секущей плоскости, смотрит от наблюдателя
                    // и материалом разреза не является: у среза нормаль наружу направлена к наблюдателю.
                    gp_Dir outward = found.Axis().Direction();
                    if (face.Orientation() == TopAbs_REVERSED) outward.Reverse();
                    if (outward.Dot(plane.Axis().Direction()) < 0.0) continue;
                    SectionArea area;
                    area.body = body;
                    for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                        std::vector<Point2> loop;
                        for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
                            BRepAdaptor_Curve curve(e.Current());
                            std::vector<gp_Pnt> points;
                            if (curve.GetType() == GeomAbs_Line) {
                                points = {curve.Value(curve.FirstParameter()), curve.Value(curve.LastParameter())};
                            } else {
                                GCPnts_QuasiUniformDeflection sampler(curve, 0.02);
                                if (!sampler.IsDone()) continue;
                                for (int i = 1; i <= sampler.NbPoints(); ++i) points.push_back(sampler.Value(i));
                            }
                            if (e.Current().Orientation() == TopAbs_REVERSED) std::reverse(points.begin(), points.end());
                            for (const gp_Pnt& p : points) {
                                const Point2 q = flat(p);
                                if (!loop.empty() && std::hypot(loop.back().x - q.x, loop.back().y - q.y) < 1e-7) continue;
                                loop.push_back(q);
                            }
                        }
                        if (loop.size() > 1 && std::hypot(loop.back().x - loop.front().x, loop.back().y - loop.front().y) < 1e-7)
                            loop.pop_back();
                        if (loop.size() >= 3) area.loops.push_back(std::move(loop));
                    }
                    if (!area.loops.empty()) {
                        view.sectionAreas.push_back(std::move(area));
                        capBuilder.Add(caps, face);
                    }
                }
            }
            if (view.sectionAreas.empty())
                return makeError(ErrorCode::InvalidState, "секущая плоскость не проходит через материал тела");
            // Линии: у разреза — всё, что за плоскостью, и нерассечённые тела; у сечения — только сам срез.
            ShapePtr shown = rest;
            if (onlyCut) {
                auto only = std::make_shared<OcctShape>();
                only->shape = caps;
                shown = only;
            } else if (kept) {
                auto both = std::make_shared<OcctShape>();
                TopoDS_Compound compound;
                TopoDS_Builder builder;
                builder.MakeCompound(compound);
                builder.Add(compound, rest->shape);
                builder.Add(compound, kept->shape);
                both->shape = compound;
                shown = both;
            }
            auto projected = project(shown, viewDirection, up, withHidden && !onlyCut);
            if (!projected) return projected;
            view.curves = projected.take().curves;
            return view;
        } catch (const Standard_Failure& e) {
            return failure("построение разреза", e);
        }
    }

    Result<Bounds3> bounds(const ShapePtr& shape) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "нет тела");
        try {
            Bnd_Box box;
            BRepBndLib::AddOptimal(s->shape, box, false, false);
            if (box.IsVoid()) return makeError(ErrorCode::InvalidState, "тело пусто");
            Bounds3 out;
            box.Get(out.min.x, out.min.y, out.min.z, out.max.x, out.max.y, out.max.z);
            return out;
        } catch (const Standard_Failure& e) {
            return failure("габарит тела", e);
        }
    }

    Result<void> exportStep(const std::vector<ShapePtr>& shapes, const std::filesystem::path& path) override {
        try {
            STEPControl_Writer writer;
            for (const auto& shape : shapes) {
                const auto s = cast(shape);
                if (!s) return makeError(ErrorCode::InvalidArgument, "экспорт STEP: нет тела");
                if (writer.Transfer(s->shape, STEPControl_AsIs) != IFSelect_RetDone)
                    return makeError(ErrorCode::Internal, "экспорт STEP: тело не удалось преобразовать");
            }
            if (writer.Write(path.string().c_str()) != IFSelect_RetDone)
                return makeError(ErrorCode::IoError, "экспорт STEP: не удалось записать файл " + path.string());
            return {};
        } catch (const Standard_Failure& e) {
            return failure("экспорт STEP", e);
        }
    }

    Result<ShapePtr> importStep(const std::filesystem::path& path, const std::string& tag) override {
        try {
            STEPControl_Reader reader;
            if (reader.ReadFile(path.string().c_str()) != IFSelect_RetDone)
                return makeError(ErrorCode::IoError, "импорт STEP: не удалось прочитать файл " + path.string());
            reader.TransferRoots();
            auto out = std::make_shared<OcctShape>();
            out->shape = reader.OneShape();
            if (out->shape.IsNull()) return makeError(ErrorCode::ParseError, "импорт STEP: в файле нет тел");
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(out->shape, TopAbs_FACE, faces);
            for (int i = 1; i <= faces.Extent(); ++i) out->setName(faces(i), tag + "/face/" + std::to_string(i));
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return failure("импорт STEP", e);
        }
    }

    Result<void> exportStl(const ShapePtr& shape, const std::filesystem::path& path, double linearDeflection) override {
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, "экспорт STL: нет тела");
        try {
            BRepMesh_IncrementalMesh mesher(s->shape, linearDeflection, false, 0.3, false);
            StlAPI_Writer writer;
            writer.ASCIIMode() = false;
            if (!writer.Write(s->shape, path.string().c_str()))
                return makeError(ErrorCode::IoError, "экспорт STL: не удалось записать файл " + path.string());
            return {};
        } catch (const Standard_Failure& e) {
            return failure("экспорт STL", e);
        }
    }

private:
    Result<ShapePtr> blend(const ShapePtr& shape, const std::vector<EdgeRef>& edges, double size,
                           const std::string& tag, bool round) {
        const char* what = round ? "скругление" : "фаска";
        const auto s = cast(shape);
        if (!s) return makeError(ErrorCode::InvalidArgument, std::string(what) + ": нет тела");
        if (size <= kLinearTolerance)
            return makeError(ErrorCode::InvalidArgument, std::string(what) + ": размер должен быть больше нуля");
        if (edges.empty()) return makeError(ErrorCode::InvalidArgument, std::string(what) + ": не выбрано ни одного ребра");
        try {
            std::unique_ptr<BRepFilletAPI_MakeFillet> fillet;
            std::unique_ptr<BRepFilletAPI_MakeChamfer> chamfer;
            if (round)
                fillet = std::make_unique<BRepFilletAPI_MakeFillet>(s->shape);
            else
                chamfer = std::make_unique<BRepFilletAPI_MakeChamfer>(s->shape);
            std::vector<std::vector<TopoDS_Edge>> perRef;
            for (const auto& ref : edges) {
                auto found = findEdges(*s, ref);
                if (found.empty())
                    return makeError(ErrorCode::NotFound, std::string(what) + ": ребро между гранями «" + ref.faceA +
                                                              "» и «" + ref.faceB + "» не найдено");
                for (const auto& edge : found) {
                    if (round)
                        fillet->Add(size, edge);
                    else
                        chamfer->Add(size, edge);
                }
                perRef.push_back(std::move(found));
            }
            BRepBuilderAPI_MakeShape& builder =
                round ? static_cast<BRepBuilderAPI_MakeShape&>(*fillet) : static_cast<BRepBuilderAPI_MakeShape&>(*chamfer);
            builder.Build();
            if (!builder.IsDone())
                return makeError(ErrorCode::InvalidArgument,
                                 std::string(what) + ": не удалось построить — размер слишком велик для этого ребра");
            auto out = std::make_shared<OcctShape>();
            out->shape = builder.Shape();
            if (auto ok = checkSolid(out->shape, what); !ok) return ok.error();
            TopTools_IndexedMapOfShape resultFaces;
            TopExp::MapShapes(out->shape, TopAbs_FACE, resultFaces);
            for (std::size_t i = 0; i < perRef.size(); ++i) {
                std::vector<TopoDS_Shape> generated;
                for (const auto& edge : perRef[i])
                    for (const TopoDS_Shape& g : builder.Generated(edge))
                        if (g.ShapeType() == TopAbs_FACE && resultFaces.Contains(g)) generated.push_back(g);
                if (!generated.empty())
                    assignNames(*out, tag + "/fillet/" + std::to_string(i + 1), std::move(generated));
            }
            propagateNames(builder, {s.get()}, *out);
            nameRemaining(*out, tag);
            return ShapePtr(out);
        } catch (const Standard_Failure& e) {
            return makeError(ErrorCode::InvalidArgument,
                             std::string(what) + ": не удалось построить — размер слишком велик для этого ребра" +
                                 (e.GetMessageString() && *e.GetMessageString()
                                      ? std::string(" (") + e.GetMessageString() + ")"
                                      : std::string()));
        }
    }
};

}  // namespace

std::unique_ptr<IGeometryKernel> createOcctKernel() {
    // OCCT по умолчанию печатает ход экспорта в консоль; программе это не нужно.
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
    return std::make_unique<OcctKernel>();
}

const char* occtVersion() { return OCC_VERSION_COMPLETE; }

}  // namespace cosmocad::geometry
