"""Проверка модуля cosmocad: запускается из набора тестов проекта (требование ОБ-11)."""
import math
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cosmocad

work = tempfile.mkdtemp(prefix="cosmocad_py_")
try:
    path = os.path.join(work, "скрипт.cosmocad")
    with cosmocad.Project(path) as project:
        part = project.new_part("Втулка")
        part.parameter("наружный", 40)
        part.parameter("отверстие", "наружный / 2")
        part.parameter("высота", 30)
        ring = part.sketch([cosmocad.circle(0, 0, "наружный / 2"), cosmocad.circle(0, 0, "отверстие / 2", hole=True)])
        part.extrude(ring, "высота")
        part.material("АМг6")
        info = part.info()
        expected = math.pi * (20 ** 2 - 10 ** 2) * 30
        assert abs(info["volume_mm3"] - expected) < 1e-3, info
        assert abs(info["mass_g"] - expected / 1000 * 2.85 * 0.926) < 0.05, info
        assert info["parameters"]["отверстие"] == 20, info
        assert not info["failures"], info

        # Параметр меняется — модель перестраивается.
        part.parameter("наружный", 60)
        assert abs(part.info()["volume_mm3"] - math.pi * (30 ** 2 - 15 ** 2) * 30) < 1e-3

        # Сборка с крепежом из библиотеки.
        nut = project.fastener("nut", 12)
        assert nut == "Гайка М12 ГОСТ 5915-70", nut
        unit = project.new_assembly("Узел")
        unit.add("Втулка")
        unit.add(nut, at=(100, 0, 0))
        unit.add(nut, at=(140, 0, 0))
        bom = {line["name"]: line["quantity"] for line in unit.info()["bill_of_materials"]}
        assert bom == {"Втулка": 1, "Гайка М12 ГОСТ 5915-70": 2}, bom
        project.export_step("Узел", os.path.join(work, "узел.step"))

        # Полипропиленовая труба: расчёт и деталь.
        check = project.pipe_check(25, 0.6, "1", sdr=6)
        assert check["fits"] and abs(check["wall"] - 4.2) < 1e-9, check
        assert abs(check["stress_mpa"] - 0.6 * (25 - 4.2) / 8.4) < 1e-9, check
        assert not project.pipe_check(25, 1.0, "5", sdr=11)["fits"]
        pipe = project.pipe(25, 1000, sdr=6)
        assert pipe == "Труба PP-R 25×4,2 L=1000", pipe
        volume = project.part(pipe).info()["volume_mm3"]
        assert abs(volume - math.pi * (12.5 ** 2 - 8.3 ** 2) * 1000) < 1.0, volume
        # Уголок из библиотеки проката.
        corner = project.angle(50, 5, 1000)
        assert corner == "Уголок 50×50×5 ГОСТ 8509-93 L=1000", corner
        volume = project.part(corner).info()["volume_mm3"]
        assert abs(volume - 480.0 * 1000) < 0.005 * 480.0 * 1000, volume  # площадь сечения 4,80 см² по таблице
        beam = project.beam("20Б1", 500)
        assert beam == "Двутавр 20Б1 ГОСТ Р 57837-2017 L=500", beam
        volume = project.part(beam).info()["volume_mm3"]
        assert abs(volume - 2716.0 * 500) < 0.006 * 2716.0 * 500, volume  # площадь сечения 27,16 см² по таблице
        assert project.channel("16П", 500) == "Швеллер 16П ГОСТ 8240-97 L=500"
        try:
            project.pipe(25, 1000)
            raise AssertionError("труба без серии и стенки должна быть отклонена")
        except cosmocad.CosmoCadError:
            pass
    assert os.path.getsize(os.path.join(work, "узел.step")) > 2000

    # Проект сохранён: новый запуск видит документы и продолжает работу.
    again = cosmocad.Project(path)
    names = sorted(d["name"] for d in again.documents())
    assert names == ["Втулка", "Гайка М12 ГОСТ 5915-70", "Двутавр 20Б1 ГОСТ Р 57837-2017 L=500", "Труба PP-R 25×4,2 L=1000",
                     "Уголок 50×50×5 ГОСТ 8509-93 L=1000", "Узел", "Швеллер 16П ГОСТ 8240-97 L=500"], names
    assert abs(again.part("Втулка").info()["parameters"]["наружный"] - 60) < 1e-9

    # Ошибка программы — исключение с её объяснением; файл проекта остаётся прежним.
    broken = cosmocad.Project(path)
    broken.part("Втулка").parameter("высота", "нет_такого_параметра * 2")
    try:
        broken.run()
        raise AssertionError("ошибка не замечена")
    except cosmocad.CosmoCadError as error:
        assert "команда 1" in str(error) and "нет_такого_параметра" in str(error), str(error)
    assert abs(cosmocad.Project(path).part("Втулка").info()["parameters"]["высота"] - 30) < 1e-9
    try:
        cosmocad.Project(path).part("Нет такой").info()
        raise AssertionError("ошибка не замечена")
    except cosmocad.CosmoCadError as error:
        assert "Нет такой" in str(error)

    # Пример из папки examples выполняется целиком.
    example = os.path.join(os.path.dirname(os.path.abspath(__file__)), "examples", "plate_with_bolts.py")
    result = subprocess.run([sys.executable, example, work], capture_output=True, text=True, encoding="utf-8")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Болт М8×30 ГОСТ 7798-70" in result.stdout and "4 шт." in result.stdout, result.stdout
    assert os.path.exists(os.path.join(work, "плита_с_болтами.step"))
    print("Скрипты Python: всё в порядке")
finally:
    shutil.rmtree(work, ignore_errors=True)
