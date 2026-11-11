// Трубы из полипропилена: напряжение, допустимое давление, срок службы, отрезок трубы.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "AppContext.h"

namespace fs = std::filesystem;
using namespace cosmocad;

namespace {

struct Pipes : ::testing::Test {
    std::unique_ptr<app::AppContext> context = app::AppContext::create(fs::path(COSMOCAD_TEST_RESOURCES)).take();
    const piping::PipeLibrary& pipes() { return context->pipes(); }
};

TEST_F(Pipes, WallThicknessFollowsSeries) {
    // Толщины стенок — таблица 1 ГОСТ 32415-2013.
    EXPECT_DOUBLE_EQ(pipes().wall(20, 5).value(), 4.1);
    EXPECT_DOUBLE_EQ(pipes().wall(20, 6).value(), 3.4);
    EXPECT_DOUBLE_EQ(pipes().wall(20, 7.4).value(), 2.8);
    EXPECT_DOUBLE_EQ(pipes().wall(20, 11).value(), 1.9);
    EXPECT_DOUBLE_EQ(pipes().wall(25, 7.4).value(), 3.5);
    EXPECT_DOUBLE_EQ(pipes().wall(32, 11).value(), 2.9);
    EXPECT_DOUBLE_EQ(pipes().wall(50, 6).value(), 8.3);
    EXPECT_DOUBLE_EQ(pipes().wall(63, 7.4).value(), 8.6);
    EXPECT_DOUBLE_EQ(pipes().wall(75, 11).value(), 6.8);
    EXPECT_DOUBLE_EQ(pipes().wall(160, 11).value(), 14.6);
    EXPECT_DOUBLE_EQ(pipes().wall(110, 5).value(), 22.1);
    EXPECT_DOUBLE_EQ(pipes().wall(110, 6).value(), 18.3);
    EXPECT_DOUBLE_EQ(pipes().wall(110, 7.4).value(), 15.1);
    EXPECT_DOUBLE_EQ(pipes().wall(110, 11).value(), 10.0);
    EXPECT_DOUBLE_EQ(pipes().wall(16, 11).value(), 1.8);  // у тонких труб стенка не меньше 1,8 мм
    EXPECT_DOUBLE_EQ(pipes().wall(16, 9).value(), 1.8);
    EXPECT_DOUBLE_EQ(pipes().wall(140, 6).value(), 23.3);
    EXPECT_DOUBLE_EQ(pipes().wall(160, 5).value(), 32.1);
    EXPECT_FALSE(pipes().wall(21, 6));
    EXPECT_FALSE(pipes().wall(20, 8));
}

TEST_F(Pipes, HoopStressFormulaAndBadInput) {
    // σ = p·(d − e) / (2e): 1 МПа в трубе 25×4,2.
    EXPECT_NEAR(piping::PipeLibrary::hoopStress(1.0, 25, 4.2).value(), 20.8 / 8.4, 1e-12);
    EXPECT_DOUBLE_EQ(piping::PipeLibrary::hoopStress(0.0, 25, 4.2).value(), 0.0);
    EXPECT_FALSE(piping::PipeLibrary::hoopStress(1.0, 25, 12.5));
    EXPECT_FALSE(piping::PipeLibrary::hoopStress(1.0, 0, 4));
    EXPECT_FALSE(piping::PipeLibrary::hoopStress(-1.0, 25, 4.2));
    EXPECT_FALSE(piping::PipeLibrary::hoopStress(std::nan(""), 25, 4.2));
}

TEST_F(Pipes, StrengthCurveMatchesKnownPoints) {
    // PP-R, 20 °C, 50 лет: длительная прочность около 9,7 МПа (с запасом 1,4 — 6,93 МПа).
    const double hours = pipes().hoursToFailure("PP-R", 20, 9.7).value();
    EXPECT_NEAR(hours / 8760.0, 50.0, 8.0);
    // Чем горячее и чем больше напряжение, тем меньше времени.
    EXPECT_GT(pipes().hoursToFailure("PP-R", 60, 4.0).value(), pipes().hoursToFailure("PP-R", 80, 4.0).value());
    EXPECT_GT(pipes().hoursToFailure("PP-R", 70, 3.0).value(), pipes().hoursToFailure("PP-R", 70, 4.0).value());
    EXPECT_GT(pipes().hoursToFailure("PP-RCT", 70, 4.0).value(), pipes().hoursToFailure("PP-R", 70, 4.0).value());
    EXPECT_FALSE(pipes().hoursToFailure("PP-R", 5, 4.0));
    EXPECT_FALSE(pipes().hoursToFailure("PP-R", 120, 4.0));
    EXPECT_FALSE(pipes().hoursToFailure("PP-R", 60, 0.0));
    EXPECT_FALSE(pipes().hoursToFailure("PVC", 60, 4.0));
}

TEST_F(Pipes, DesignStressByServiceClass) {
    // Таблицы Г.2–Г.5 ГОСТ 32415-2013: классы 1, 2, 4, 5 и холодная вода.
    const std::vector<std::string> classes = {"1", "2", "4", "5", "ХВ"};
    const std::map<std::string, std::vector<double>> table = {{"PP-H", {2.90, 1.99, 3.24, 1.83, 6.25}},
                                                              {"PP-B", {1.67, 1.19, 1.95, 1.19, 6.21}},
                                                              {"PP-R", {3.09, 2.13, 3.30, 1.90, 6.93}},
                                                              {"PP-RCT", {3.64, 3.40, 3.67, 2.92, 8.25}}};
    for (const auto& [material, values] : table)
        for (std::size_t i = 0; i < classes.size(); ++i) {
            EXPECT_DOUBLE_EQ(pipes().designStress(material, classes[i]).value(), values[i]) << material << " " << classes[i];
            // Расчёт по правилу Майнера из уравнений приложения В сходится с таблицей до 0,02 МПа;
            // исключение — PP-R в классе 1: в таблице 3,09, по уравнениям 3,02.
            const double tolerance = material == "PP-R" && classes[i] == "1" ? 0.08 : 0.03;
            EXPECT_NEAR(pipes().computedDesignStress(material, classes[i]).value(), values[i], tolerance) << material << " " << classes[i];
        }
    EXPECT_DOUBLE_EQ(pipes().designStress("PP-R", "XB").value(), 6.93);  // латиницей
    EXPECT_FALSE(pipes().designStress("PP-R", "3"));
    // При вычисленном допустимом напряжении срок службы равен расчётному.
    const double design = pipes().computedDesignStress("PP-R", "2").value();
    EXPECT_NEAR(pipes().lifeYears("PP-R", "2", design).value(), 50.0, 0.01);
    EXPECT_LT(pipes().lifeYears("PP-R", "2", design * 1.2).value(), 50.0);
    EXPECT_DOUBLE_EQ(pipes().lifeYears("PP-R", "2", 0.01).value(), piping::kLifeLimitYears);
}

TEST_F(Pipes, MaxSeriesMatchesStandardTables) {
    // Наибольшая серия для давлений 0,4; 0,6; 0,8; 1,0 МПа — таблицы Г.2–Г.5.
    const std::vector<double> pressures = {0.4, 0.6, 0.8, 1.0};
    const std::vector<std::string> classes = {"1", "2", "4", "5"};
    const std::map<std::string, std::vector<std::vector<double>>> table = {
        {"PP-H", {{6.3, 4.8, 3.6, 2.9}, {5.0, 3.3, 2.5, 2.0}, {6.3, 5.4, 4.1, 3.2}, {4.6, 3.0, 2.3, 1.8}}},
        {"PP-B", {{4.2, 2.8, 2.1, 1.7}, {3.0, 2.0, 1.5, 1.2}, {4.9, 3.3, 2.4, 2.0}, {3.0, 2.0, 1.5, 1.2}}},
        {"PP-R", {{6.9, 5.2, 3.9, 3.1}, {5.3, 3.6, 2.7, 2.1}, {6.9, 5.5, 4.1, 3.3}, {4.8, 3.2, 2.4, 1.9}}},
        {"PP-RCT", {{8.2, 6.1, 4.5, 3.6}, {8.3, 5.7, 4.3, 3.4}, {8.3, 6.1, 4.6, 3.7}, {7.3, 4.9, 3.7, 2.9}}}};
    for (const auto& [material, rows] : table)
        for (std::size_t c = 0; c < classes.size(); ++c)
            for (std::size_t p = 0; p < pressures.size(); ++p)
                EXPECT_NEAR(pipes().maxSeries(material, classes[c], pressures[p]).value(), rows[c][p], 1e-9)
                    << material << " класс " << classes[c] << " " << pressures[p];
    EXPECT_NEAR(pipes().maxSeries("PP-R", "ХВ", 1.0).value(), 6.9, 1e-9);
    // Давление не из ряда стандарта — серия вычисляется: 3,09 / 0,5 = 6,18 → 6,2.
    EXPECT_NEAR(pipes().maxSeries("PP-R", "1", 0.5).value(), 6.2, 1e-9);
    EXPECT_FALSE(pipes().maxSeries("PP-R", "1", 0.0));
}

TEST_F(Pipes, ExampleFromTheStandardItself) {
    // Пример обозначения из п. 4.4: «Труба PP-R SDR 6/S 2,5 - 20х3,4 класс 2/0,8 МПа класс 5/0,6 МПа».
    EXPECT_TRUE(pipes().check("PP-R", "2", 20, 3.4, 0.8).value().ok);
    EXPECT_TRUE(pipes().check("PP-R", "5", 20, 3.4, 0.6).value().ok);
    EXPECT_FALSE(pipes().check("PP-R", "2", 20, 3.4, 1.0).value().ok);
    EXPECT_FALSE(pipes().check("PP-R", "5", 20, 3.4, 0.8).value().ok);
    EXPECT_DOUBLE_EQ(pipes().check("PP-R", "2", 20, 3.4, 0.8).value().nominalSeries, 2.5);
}

TEST_F(Pipes, CheckGivesVerdictAndReport) {
    auto good = pipes().check("PP-R", "1", 25, 4.2, 0.6);
    ASSERT_TRUE(good);
    EXPECT_TRUE(good.value().ok);
    EXPECT_NEAR(good.value().series, 20.8 / 8.4, 1e-12);
    EXPECT_DOUBLE_EQ(good.value().nominalSeries, 2.5);
    EXPECT_TRUE(good.value().tableStress);
    EXPECT_NEAR(good.value().allowedPressure, 3.09 / 2.5, 1e-12);
    EXPECT_GT(good.value().lifeYears, 50.0);
    EXPECT_LT(good.value().minimumWall, 4.2);
    // Наименьшая стенка — та, при которой напряжение равно допустимому.
    const double e = good.value().minimumWall;
    EXPECT_NEAR(piping::PipeLibrary::hoopStress(0.6, 25, e).value(), good.value().designStress, 1e-9);
    EXPECT_NE(good.value().lines[good.value().lines.size() - 2].find("ГОСТ 32415-2013"), std::string::npos);

    auto bad = pipes().check("PP-R", "5", 25, 2.3, 1.0);
    ASSERT_TRUE(bad);
    EXPECT_FALSE(bad.value().ok);
    EXPECT_LT(bad.value().lifeYears, 50.0);
    EXPECT_GT(bad.value().minimumWall, 2.3);
    // Та же труба из PP-RCT держит больше.
    EXPECT_GT(pipes().check("PP-RCT", "5", 25, 2.3, 1.0).value().allowedPressure, bad.value().allowedPressure);
    // На границе: допустимое давление проходит, чуть больше — нет.
    const double limit = good.value().allowedPressure;
    EXPECT_TRUE(pipes().check("PP-R", "1", 25, 4.2, limit).value().ok);
    EXPECT_FALSE(pipes().check("PP-R", "1", 25, 4.2, limit * 1.05).value().ok);
    // Тонкая труба не из ряда: рабочее давление держит, но холодную воду 1,0 МПа — нет.
    auto thin = pipes().check("PP-R", "1", 25, 1.5, 0.3);
    ASSERT_TRUE(thin);
    EXPECT_DOUBLE_EQ(thin.value().nominalSeries, 0.0);
    EXPECT_FALSE(thin.value().coldOk);
    EXPECT_FALSE(thin.value().ok);
    EXPECT_LE(0.3, thin.value().allowedPressure);
    EXPECT_FALSE(pipes().check("PP-R", "1", 25, 13, 0.6));
    EXPECT_FALSE(pipes().check("PP-R", "нет", 25, 4.2, 0.6));
}

TEST_F(Pipes, PipePartIsBuiltOnceAndBadOneLeavesNothing) {
    const std::size_t before = context->documents().project().documents().size();
    auto part = context->pipePart("PP-R", 32, 5.4, 500);
    ASSERT_TRUE(part);
    EXPECT_EQ(part.value()->name, "Труба PP-R 32×5,4 L=500");
    const auto body = context->modeling().body(*part.value());
    ASSERT_TRUE(body);
    const double expected = std::numbers::pi * (16.0 * 16.0 - 10.6 * 10.6) * 500.0;
    EXPECT_NEAR(context->kernel().massProperties(body).value().volume, expected, 1e-3 * expected);
    // Повторный запрос возвращает ту же деталь; неверные размеры ничего не оставляют в проекте.
    EXPECT_EQ(context->pipePart("PP-R", 32, 5.4, 500).value(), part.value());
    EXPECT_FALSE(context->pipePart("PP-R", 32, 16, 500));
    EXPECT_FALSE(context->pipePart("PP-R", 32, 5.4, 0));
    EXPECT_FALSE(context->pipePart("сталь", 32, 5.4, 500));
    EXPECT_EQ(context->documents().project().documents().size(), before + 1);
}

}  // namespace
