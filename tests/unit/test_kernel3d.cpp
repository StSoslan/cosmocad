// Геометрическое ядро (адаптер OCCT) и устойчивые имена граней (требование 3D-11).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <numbers>

#include "cosmocad/geometry/OcctKernel.h"

using namespace cosmocad;
using namespace cosmocad::geometry;

namespace {

Profile2D rectangle(double x, double y, double w, double h, std::uint64_t tagBase = 100) {
    const Point2 p[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    Profile2D profile;
    for (int i = 0; i < 4; ++i) {
        ProfileSegment s;
        s.start = p[i];
        s.end = p[(i + 1) % 4];
        s.sourceTag = tagBase + static_cast<std::uint64_t>(i);
        profile.outer.push_back(s);
    }
    return profile;
}

Profile2D circle(double cx, double cy, double r, std::uint64_t tag = 200) {
    ProfileSegment s;
    s.kind = ProfileSegment::Kind::Circle;
    s.center = {cx, cy};
    s.start = {cx + r, cy};
    s.sourceTag = tag;
    Profile2D profile;
    profile.outer.push_back(s);
    return profile;
}

bool has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Пластина w×50×10 с отверстием радиуса r: то, что будет перестраиваться при изменении размеров.
ShapePtr plateWithHole(IGeometryKernel& kernel, double w, double r) {
    auto plate = kernel.extrude(rectangle(0, 0, w, 50), Plane{}, 10, "plate");
    EXPECT_TRUE(plate.ok());
    auto tool = kernel.extrude(circle(w / 2, 25, r), Plane{}, 10, "hole");
    EXPECT_TRUE(tool.ok());
    auto cut = kernel.boolean(BooleanKind::Subtract, plate.value(), tool.value());
    EXPECT_TRUE(cut.ok());
    return cut.value();
}

}  // namespace

TEST(Kernel3D, ExtrudeGivesBoxWithNamedFaces) {
    auto kernel = createOcctKernel();
    auto box = kernel->extrude(rectangle(0, 0, 80, 50), Plane{}, 10, "plate");
    ASSERT_TRUE(box.ok()) << box.error().message;
    const auto names = kernel->faceNames(box.value());
    EXPECT_EQ(names.size(), 6u);
    for (const char* name : {"plate/start", "plate/end", "plate/side/100", "plate/side/101", "plate/side/102",
                             "plate/side/103"})
        EXPECT_TRUE(has(names, name)) << name;
    const auto mass = kernel->massProperties(box.value());
    ASSERT_TRUE(mass.ok());
    EXPECT_NEAR(mass.value().volume, 80.0 * 50.0 * 10.0, 1e-6);
    EXPECT_NEAR(mass.value().centerOfMass.z, 5.0, 1e-9);

    const auto top = kernel->faceInfo(box.value(), "plate/end");
    ASSERT_TRUE(top.ok());
    EXPECT_EQ(top.value().kind, FaceInfo::Kind::Plane);
    EXPECT_NEAR(top.value().direction.z, 1.0, 1e-9);  // нормаль наружу
    const auto bottom = kernel->faceInfo(box.value(), "plate/start");
    ASSERT_TRUE(bottom.ok());
    EXPECT_NEAR(bottom.value().direction.z, -1.0, 1e-9);
}

TEST(Kernel3D, NegativeDistanceExtrudesDownAndStaysValid) {
    auto kernel = createOcctKernel();
    auto box = kernel->extrude(rectangle(0, 0, 10, 10), Plane{}, -5, "b");
    ASSERT_TRUE(box.ok());
    EXPECT_TRUE(kernel->validate(box.value()).ok());
    const auto mass = kernel->massProperties(box.value());
    EXPECT_NEAR(mass.value().volume, 500.0, 1e-6);
    EXPECT_NEAR(mass.value().centerOfMass.z, -2.5, 1e-9);
}

TEST(Kernel3D, CutKeepsFaceNamesAndAddsHoleWall) {
    auto kernel = createOcctKernel();
    const auto body = plateWithHole(*kernel, 80, 12);
    const auto names = kernel->faceNames(body);
    EXPECT_EQ(names.size(), 7u);
    EXPECT_TRUE(has(names, "plate/end"));
    EXPECT_TRUE(has(names, "hole/side/200"));
    const auto wall = kernel->faceInfo(body, "hole/side/200");
    ASSERT_TRUE(wall.ok());
    EXPECT_EQ(wall.value().kind, FaceInfo::Kind::Cylinder);
    EXPECT_NEAR(wall.value().radius, 12.0, 1e-9);
    const auto mass = kernel->massProperties(body);
    EXPECT_NEAR(mass.value().volume, 80.0 * 50 * 10 - std::numbers::pi * 144 * 10, 1e-4);
}

// Главная проверка 3D-11: ссылка на ребро переживает изменение размеров модели.
TEST(Kernel3D, EdgeReferenceSurvivesDimensionChange) {
    auto kernel = createOcctKernel();
    const EdgeRef topRim{"plate/end", "hole/side/200"};      // кромка отверстия сверху
    const EdgeRef sideEdge{"plate/side/101", "plate/end"};   // ребро правой стороны сверху
    for (const auto& [w, r] : {std::pair{80.0, 12.0}, std::pair{120.0, 5.0}, std::pair{60.0, 20.0}}) {
        const auto body = plateWithHole(*kernel, w, r);
        ASSERT_TRUE(kernel->hasEdge(body, topRim)) << w;
        ASSERT_TRUE(kernel->hasEdge(body, sideEdge)) << w;
        auto rounded = kernel->fillet(body, {topRim, sideEdge}, 2.0, "f");
        ASSERT_TRUE(rounded.ok()) << rounded.error().message;
        const auto names = kernel->faceNames(rounded.value());
        EXPECT_TRUE(has(names, "f/fillet/1"));
        EXPECT_TRUE(has(names, "f/fillet/2"));
        EXPECT_TRUE(has(names, "plate/end"));       // изменённая грань сохранила имя
        EXPECT_TRUE(has(names, "hole/side/200"));
        // Правая сторона после скругления осталась справа: x её центра равен ширине.
        const auto right = kernel->faceInfo(rounded.value(), "plate/side/101");
        ASSERT_TRUE(right.ok());
        EXPECT_NEAR(right.value().centroid.x, w, 1e-6);
        EXPECT_TRUE(kernel->validate(rounded.value()).ok());
    }
}

TEST(Kernel3D, SameInputGivesSameNames) {
    auto kernel = createOcctKernel();
    const auto a = kernel->faceNames(plateWithHole(*kernel, 80, 12));
    const auto b = kernel->faceNames(plateWithHole(*kernel, 80, 12));
    EXPECT_EQ(a, b);
}

TEST(Kernel3D, FaceSplitByCutGetsNumberedNames) {
    auto kernel = createOcctKernel();
    auto plate = kernel->extrude(rectangle(0, 0, 80, 50), Plane{}, 10, "plate");
    // Паз во всю ширину режет верхнюю грань на две части.
    Plane top;
    top.origin = {0, 0, 5};
    auto slot = kernel->extrude(rectangle(30, -5, 20, 60, 300), top, 10, "slot");
    auto cut = kernel->boolean(BooleanKind::Subtract, plate.value(), slot.value());
    ASSERT_TRUE(cut.ok()) << cut.error().message;
    const auto names = kernel->faceNames(cut.value());
    ASSERT_TRUE(has(names, "plate/end"));
    ASSERT_TRUE(has(names, "plate/end#2"));
    // Части пронумерованы по координате: первая левее второй.
    EXPECT_LT(kernel->faceInfo(cut.value(), "plate/end").value().centroid.x,
              kernel->faceInfo(cut.value(), "plate/end#2").value().centroid.x);
    EXPECT_TRUE(has(names, "slot/start"));  // дно паза
}

TEST(Kernel3D, FilletTooLargeIsReportedNotCrashed) {
    auto kernel = createOcctKernel();
    auto box = kernel->extrude(rectangle(0, 0, 10, 10), Plane{}, 4, "b");
    auto bad = kernel->fillet(box.value(), {{"b/side/100", "b/end"}}, 50.0, "f");
    EXPECT_FALSE(bad.ok());
    auto missing = kernel->fillet(box.value(), {{"b/side/100", "нет такой"}}, 1.0, "f");
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, core::ErrorCode::NotFound);
}

TEST(Kernel3D, ChamferRemovesExpectedVolume) {
    auto kernel = createOcctKernel();
    auto box = kernel->extrude(rectangle(0, 0, 10, 10), Plane{}, 10, "b");
    auto cut = kernel->chamfer(box.value(), {{"b/side/100", "b/end"}}, 2.0, "c");
    ASSERT_TRUE(cut.ok()) << cut.error().message;
    EXPECT_NEAR(kernel->massProperties(cut.value()).value().volume, 1000.0 - 2.0 * 2.0 / 2.0 * 10.0, 1e-6);
}

TEST(Kernel3D, RevolveMakesCylinderAndPartialRevolveHasEnds) {
    auto kernel = createOcctKernel();
    // Прямоугольник 10×30 в плоскости XY, вращение вокруг оси Y: цилиндр радиуса 10 высотой 30.
    auto full = kernel->revolve(rectangle(0, 0, 10, 30), Plane{}, Point3{}, Vector3{0, 1, 0}, 360, "r");
    ASSERT_TRUE(full.ok()) << full.error().message;
    EXPECT_NEAR(kernel->massProperties(full.value()).value().volume, std::numbers::pi * 100 * 30, 1e-3);
    EXPECT_TRUE(has(kernel->faceNames(full.value()), "r/side/101"));

    auto quarter = kernel->revolve(rectangle(0, 0, 10, 30), Plane{}, Point3{}, Vector3{0, 1, 0}, 90, "r");
    ASSERT_TRUE(quarter.ok());
    const auto names = kernel->faceNames(quarter.value());
    EXPECT_TRUE(has(names, "r/start"));
    EXPECT_TRUE(has(names, "r/end"));
    EXPECT_NEAR(kernel->massProperties(quarter.value()).value().volume, std::numbers::pi * 100 * 30 / 4, 1e-3);

    // Контур, пересекающий ось, — понятная ошибка, а не сбой.
    auto bad = kernel->revolve(rectangle(-5, 0, 10, 30), Plane{}, Point3{}, Vector3{0, 1, 0}, 360, "r");
    EXPECT_FALSE(bad.ok());
}

TEST(Kernel3D, BadProfilesAreRejected) {
    auto kernel = createOcctKernel();
    Profile2D open = rectangle(0, 0, 10, 10);
    open.outer.pop_back();
    EXPECT_FALSE(kernel->extrude(open, Plane{}, 5, "x").ok());
    EXPECT_FALSE(kernel->extrude(rectangle(0, 0, 10, 10), Plane{}, 0, "x").ok());
    Profile2D bow;  // «бабочка»: контур пересекает сам себя
    const Point2 p[4] = {{0, 0}, {10, 10}, {10, 0}, {0, 10}};
    for (int i = 0; i < 4; ++i) {
        ProfileSegment s;
        s.start = p[i];
        s.end = p[(i + 1) % 4];
        bow.outer.push_back(s);
    }
    EXPECT_FALSE(kernel->extrude(bow, Plane{}, 5, "x").ok());
}

TEST(Kernel3D, ProfileWithHoleAndClockwiseContour) {
    auto kernel = createOcctKernel();
    Profile2D profile = rectangle(0, 0, 40, 40);
    std::reverse(profile.outer.begin(), profile.outer.end());  // по часовой стрелке
    for (auto& s : profile.outer) std::swap(s.start, s.end);
    profile.holes.push_back(circle(20, 20, 5).outer);
    auto body = kernel->extrude(profile, Plane{}, 10, "p");
    ASSERT_TRUE(body.ok()) << body.error().message;
    EXPECT_NEAR(kernel->massProperties(body.value()).value().volume, (1600 - std::numbers::pi * 25) * 10, 1e-4);
    EXPECT_TRUE(has(kernel->faceNames(body.value()), "p/side/200"));
}

TEST(Kernel3D, TessellationIsClosedAndNamed) {
    auto kernel = createOcctKernel();
    const auto body = plateWithHole(*kernel, 80, 12);
    const auto mesh = kernel->tessellate(body, 0.1);
    ASSERT_TRUE(mesh.ok());
    EXPECT_GT(mesh.value().triangles.size(), 20u);
    EXPECT_EQ(mesh.value().triangles.size(), mesh.value().triangleFace.size());
    EXPECT_EQ(mesh.value().vertices.size(), mesh.value().normals.size());
    EXPECT_EQ(mesh.value().faceNames.size(), 7u);
    EXPECT_FALSE(mesh.value().edges.empty());
    // Объём по сетке близок к точному: сетка замкнута и смотрит наружу.
    double volume = 0.0;
    for (const auto& t : mesh.value().triangles) {
        const auto& a = mesh.value().vertices[t[0]];
        const auto& b = mesh.value().vertices[t[1]];
        const auto& c = mesh.value().vertices[t[2]];
        volume += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
    }
    EXPECT_NEAR(volume, kernel->massProperties(body).value().volume, 30.0);
}

TEST(Kernel3D, TransformMovesBodyAndKeepsNames) {
    auto kernel = createOcctKernel();
    auto box = kernel->extrude(rectangle(0, 0, 10, 10), Plane{}, 10, "b");
    const Placement turn = Placement::translation(100, 0, 0) *
                           Placement::rotation(Point3{}, Vector3{1, 0, 0}, std::numbers::pi / 2);
    auto moved = kernel->transformed(box.value(), turn);
    ASSERT_TRUE(moved.ok());
    const auto top = kernel->faceInfo(moved.value(), "b/end");
    ASSERT_TRUE(top.ok());
    EXPECT_NEAR(top.value().direction.y, -1.0, 1e-9);  // ось Z перешла в −Y
    EXPECT_NEAR(top.value().centroid.x, 105.0, 1e-9);
    EXPECT_NEAR(kernel->commonVolume(box.value(), moved.value()).value(), 0.0, 1e-9);
    auto shifted = kernel->transformed(box.value(), Placement::translation(5, 0, 0));
    EXPECT_NEAR(kernel->commonVolume(box.value(), shifted.value()).value(), 500.0, 1e-6);
}

TEST(Kernel3D, ProjectionGivesViewLines) {
    auto kernel = createOcctKernel();
    const auto body = plateWithHole(*kernel, 80, 12);
    // Вид сверху: прямоугольник 80×50 и окружность.
    auto top = kernel->project(body, Vector3{0, 0, -1}, Vector3{0, 1, 0}, false);
    ASSERT_TRUE(top.ok()) << top.error().message;
    int circles = 0, lines = 0;
    BBox2 box;
    for (const auto& c : top.value().curves) {
        if (c.kind == ProjectedCurve::Kind::Circle) {
            ++circles;
            EXPECT_NEAR(c.radius, 12.0, 1e-6);
            EXPECT_NEAR(c.center.x, 40.0, 1e-6);
        } else if (c.kind == ProjectedCurve::Kind::Line) {
            ++lines;
            box.expand(c.a);
            box.expand(c.b);
        }
    }
    EXPECT_EQ(circles, 1);
    EXPECT_EQ(lines, 4);
    EXPECT_NEAR(box.width(), 80.0, 1e-6);
    EXPECT_NEAR(box.height(), 50.0, 1e-6);
    // Вид спереди: отверстие видно только штриховыми линиями.
    auto front = kernel->project(body, Vector3{0, 1, 0}, Vector3{0, 0, 1}, true);
    ASSERT_TRUE(front.ok());
    int hidden = 0;
    for (const auto& c : front.value().curves) hidden += c.hidden ? 1 : 0;
    EXPECT_GE(hidden, 2);
    auto frontVisible = kernel->project(body, Vector3{0, 1, 0}, Vector3{0, 0, 1}, false);
    for (const auto& c : frontVisible.value().curves) EXPECT_FALSE(c.hidden);
}

TEST(Kernel3D, StepRoundTripKeepsVolumeAndStlIsWritten) {
    auto kernel = createOcctKernel();
    const auto body = plateWithHole(*kernel, 80, 12);
    const auto dir = std::filesystem::temp_directory_path() / "cosmocad_kernel_test";
    std::filesystem::create_directories(dir);
    ASSERT_TRUE(kernel->exportStep({body}, dir / "plate.step").ok());
    auto back = kernel->importStep(dir / "plate.step", "imp");
    ASSERT_TRUE(back.ok()) << back.error().message;
    EXPECT_NEAR(kernel->massProperties(back.value()).value().volume,
                kernel->massProperties(body).value().volume, 1e-3);
    EXPECT_EQ(kernel->faceNames(back.value()).size(), 7u);
    ASSERT_TRUE(kernel->exportStl(body, dir / "plate.stl", 0.1).ok());
    EXPECT_GT(std::filesystem::file_size(dir / "plate.stl"), 1000u);
    EXPECT_FALSE(kernel->importStep(dir / "нет.step", "imp").ok());
}

// ---- Показ 3D ----
#include "cosmocad/graphics/Render3D.h"

TEST(Render3D, DrawsBodyAndTellsWhichFaceIsUnderCursor) {
    auto kernel = createOcctKernel();
    const auto body = plateWithHole(*kernel, 80, 12);
    const auto mesh = kernel->tessellate(body, 0.1).value();
    std::vector<graphics::Body3D> bodies{{&mesh, 0xFFB8C4D0, {}}};

    // Вид сверху: в центре — отверстие (фон... нет, дно не видно: сквозь отверстие виден фон).
    auto top = graphics::fitCamera(bodies, 400, 300, -90, 90);
    auto image = graphics::render(bodies, top, 400, 300);
    EXPECT_EQ(image.pick(bodies, 200, 150).body, -1);                 // сквозное отверстие
    EXPECT_EQ(image.pick(bodies, 120, 150).faceName, "plate/end");    // верхняя грань
    EXPECT_EQ(image.pick(bodies, 2, 2).body, -1);                     // фон в углу
    EXPECT_EQ(image.pixels[2], 0xFFF4F6F8u);
    EXPECT_NE(image.pixels[150 * 400 + 120], 0xFFF4F6F8u);

    // Вид спереди: видна передняя боковая грань (сторона y = 0, метка 100).
    auto front = graphics::fitCamera(bodies, 400, 300, -90, 0);
    image = graphics::render(bodies, front, 400, 300);
    EXPECT_EQ(image.pick(bodies, 200, 150).faceName, "plate/side/100");

    // Выделенная грань рисуется другим цветом.
    const auto plain = graphics::render(bodies, top, 400, 300).pixels[150 * 400 + 120];
    bodies[0].highlightedFaces = {"plate/end"};
    EXPECT_NE(graphics::render(bodies, top, 400, 300).pixels[150 * 400 + 120], plain);

    // Изометрия: тело целиком в кадре — по краям картинки фон.
    auto iso = graphics::fitCamera(bodies, 400, 300, -60, 30);
    image = graphics::render(bodies, iso, 400, 300);
    for (int x = 0; x < 400; ++x) {
        EXPECT_EQ(image.bodyAt[static_cast<std::size_t>(x)], -1);
        EXPECT_EQ(image.bodyAt[static_cast<std::size_t>(299 * 400 + x)], -1);
    }
    int covered = 0;
    for (const auto b : image.bodyAt) covered += b >= 0 ? 1 : 0;
    EXPECT_GT(covered, 400 * 300 / 6);
    EXPECT_EQ(graphics::render({}, iso, 10, 10).pixels.size(), 100u);  // пустая сцена не падает
}

// Требование ОБМ-2: испорченный внешний файл даёт ошибку, а не падение программы.
TEST(Kernel3D, BrokenStepFilesAreRejectedWithoutCrash) {
    auto kernel = createOcctKernel();
    const auto dir = std::filesystem::temp_directory_path() / "cosmocad_kernel_test";
    std::filesystem::create_directories(dir);
    const auto body = plateWithHole(*kernel, 80, 12);
    ASSERT_TRUE(kernel->exportStep({body}, dir / "good.step").ok());
    std::string good;
    {
        std::FILE* f = std::fopen((dir / "good.step").string().c_str(), "rb");
        ASSERT_NE(f, nullptr);
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) good.append(buffer, n);
        std::fclose(f);
    }
    const auto write = [&](const std::string& name, const std::string& content) {
        std::FILE* f = std::fopen((dir / name).string().c_str(), "wb");
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
        return dir / name;
    };
    EXPECT_FALSE(kernel->importStep(write("empty.step", ""), "i").ok());
    EXPECT_FALSE(kernel->importStep(write("garbage.step", std::string(5000, '\x7f')), "i").ok());
    // Обрезанный и побитый файл: ответ может быть любым, но не падение и не некорректное тело.
    unsigned seed = 12345;
    for (int round = 0; round < 12; ++round) {
        std::string broken = good.substr(0, good.size() * static_cast<std::size_t>(round + 1) / 14);
        for (int k = 0; k < 20 && !broken.empty(); ++k) {
            seed = seed * 1103515245u + 12345u;
            broken[seed % broken.size()] = static_cast<char>(seed >> 16);
        }
        auto result = kernel->importStep(write("broken.step", broken), "i");
        if (result.ok()) (void)kernel->massProperties(result.value());
    }
}
