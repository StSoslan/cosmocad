"""Пример скрипта: плита с четырьмя отверстиями, болты из библиотеки, сборка, экспорт в STEP.

Запуск:  COSMOCAD_CLI=путь/к/cosmocad-cli python3 plate_with_bolts.py [папка для результата]
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cosmocad

folder = sys.argv[1] if len(sys.argv) > 1 else "."
project_file = os.path.join(folder, "плита_с_болтами.cosmocad")
if os.path.isdir(project_file) or os.path.exists(project_file):
    import shutil
    shutil.rmtree(project_file, ignore_errors=True)

with cosmocad.Project(project_file) as project:
    # Плита: размеры — параметры, отверстия привязаны к ним формулами.
    plate = project.new_part("Плита")
    plate.parameter("длина", 120)
    plate.parameter("ширина", 80)
    plate.parameter("толщина", 12)
    plate.parameter("отступ", 15)
    holes = [cosmocad.circle(x, y, 4.5, hole=True)
             for x in ("отступ", "длина - отступ") for y in ("отступ", "ширина - отступ")]
    base = plate.sketch([cosmocad.rectangle(0, 0, "длина", "ширина")] + holes)
    plate.extrude(base, "толщина")
    plate.material("Сталь 45 ГОСТ 1050-2013")
    plate.designation("ПР.001.001")

    info = plate.info()
    print("Плита: объём %.0f мм³, масса %.0f г" % (info["volume_mm3"], info["mass_g"]))

    # Болты М8×30 из библиотеки — в сборку, на оси отверстий.
    bolt = project.fastener("bolt", 8, 30)
    unit = project.new_assembly("Плита с болтами")
    unit.add("Плита")
    for x in (15, 105):
        for y in (15, 65):
            unit.add(bolt, at=(x, y, 12))
    for line in unit.info()["bill_of_materials"]:
        print("%-28s %d шт." % (line["designation"] or line["name"], line["quantity"]))

    # Исполнение подлиннее и экспорт.
    plate.save_configuration("Базовое")
    plate.parameter("длина", 160)
    plate.save_configuration("Длинное", "ПР.001.001-01")
    print("Длинное исполнение: масса %.0f г" % plate.info()["mass_g"])
    plate.apply_configuration("Базовое")
    project.export_step("Плита с болтами", os.path.join(folder, "плита_с_болтами.step"))

print("Готово:", project_file)
