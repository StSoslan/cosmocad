// Замер на эталонной нагрузке из требования КАЧ-6: чертёж из 100 000 объектов.
// Не входит в набор тестов; запускается вручную: build/release/tests/cosmocad_bench
#include <memory>
#include <utility>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>

#include "AppContext.h"
#include "cosmocad/document/Transaction.h"
#include "cosmocad/exchange/Exchange.h"
#include "cosmocad/graphics/Scene.h"

using namespace cosmocad;
using Clock = std::chrono::steady_clock;

template <class F>
double measureMs(F&& body) {
    auto start = Clock::now();
    body();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 100000;
    auto context = app::AppContext::create(COSMOCAD_TEST_RESOURCES).take();
    document::Document& doc = *context->createDemoDrawing().value();
    auto profile = context->profileFor(doc);
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> x(25.0, 200.0), y(70.0, 285.0);

    std::cout << "Объектов: " << count << std::endl;
    std::cout << "добавление одной транзакцией, мс: " << measureMs([&] {
        document::Transaction tx(doc, "bulk");
        for (int i = 0; i < count; ++i) {
            auto line = std::make_unique<drawing::LineEntity>();
            line->start = {x(rng), y(rng)};
            line->end = {line->start.x + 1.0, line->start.y + 1.0};
            tx.add(std::move(line)).value();
        }
        tx.commit();
    }) << std::endl;

    graphics::Scene scene;
    std::cout << "построение сцены, мс: " << measureMs([&] { scene = graphics::buildScene(doc, &*profile); }) << std::endl;
    std::cout << "проверка оформления, мс: " << measureMs([&] { (void)context->standards().check(doc).take(); }) << std::endl;
    std::cout << "выбор щелчком, мс: " << measureMs([&] { (void)graphics::hitTest(doc, {100, 100}, 1.0); }) << std::endl;
    std::cout << "поиск привязки, мс: " << measureMs([&] { (void)graphics::findSnap(doc, {100, 100}, 1.0); }) << std::endl;

    std::ostringstream dxf;
    std::cout << "экспорт DXF, мс: " << measureMs([&] { (void)exchange::writeDxf(scene, dxf).ok(); }) << std::endl;

    auto path = std::filesystem::temp_directory_path() / "cosmocad-bench.cosmocad";
    std::cout << "сохранение, мс: " << measureMs([&] { (void)context->documents().saveProject(path).ok(); }) << std::endl;
    auto other = app::AppContext::create(COSMOCAD_TEST_RESOURCES).take();
    std::cout << "открытие, мс: " << measureMs([&] { (void)other->documents().openProject(path).ok(); }) << std::endl;
    std::cout << "отмена добавления, мс: " << measureMs([&] { doc.undo(); }) << std::endl;
    std::filesystem::remove_all(path);
    return 0;
}
