"""Скрипты CosmoCad на Python.

Модуль собирает список команд и передаёт его программе cosmocad-cli (команда batch).
Ничего, кроме стандартной библиотеки Python, не нужно.

Пример:

    import cosmocad

    with cosmocad.Project("плита.cosmocad") as project:
        part = project.new_part("Плита")
        part.parameter("длина", 80)
        base = part.sketch([cosmocad.rectangle(0, 0, "длина", 50)])
        part.extrude(base, 10)
        print(part.info()["mass_g"])

Команды выполняются, когда нужен ответ (info) или при выходе из блока with. Если программа
отказала, возникает ошибка CosmoCadError с её объяснением, а файл проекта остаётся прежним.
"""

import json
import os
import shutil
import subprocess
import tempfile

__all__ = ["Project", "Part", "Assembly", "CosmoCadError", "rectangle", "circle", "polygon"]


class CosmoCadError(RuntimeError):
    """Программа отказалась выполнить команду; текст ошибки — её объяснение."""


def rectangle(x, y, width, height, hole=False):
    """Прямоугольник эскиза: левый нижний угол, ширина, высота. Размеры — числа или формулы."""
    return {"kind": "rectangle", "values": [x, y, width, height], "hole": hole}


def circle(x, y, radius, hole=False):
    """Окружность эскиза: центр и радиус."""
    return {"kind": "circle", "values": [x, y, radius], "hole": hole}


def polygon(points, hole=False):
    """Замкнутая ломаная: список точек [(x, y), ...], не меньше трёх."""
    values = []
    for x, y in points:
        values += [x, y]
    return {"kind": "polygon", "values": values, "hole": hole}


def _find_cli(explicit=None):
    candidate = explicit or os.environ.get("COSMOCAD_CLI") or shutil.which("cosmocad-cli")
    if not candidate:
        raise CosmoCadError("не найдена программа cosmocad-cli: укажите путь в Project(cli=...) "
                            "или в переменной окружения COSMOCAD_CLI")
    return candidate


def _pipe(command, sdr, wall):
    if (sdr is None) == (wall is None):
        raise CosmoCadError("для трубы задайте что-то одно: серию sdr или толщину стенки wall")
    command["sdr" if wall is None else "wall"] = sdr if wall is None else wall
    return command


class Project:
    """Проект CosmoCad. Файл создаётся, если его нет."""

    def __init__(self, path, cli=None):
        self.path = str(path)
        self._cli = _find_cli(cli)
        self._pending = []
        self._names = 0

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is None:
            self.run()
        return False

    # ---- документы ----

    def new_part(self, name):
        self._add({"do": "new_part", "name": name})
        return Part(self, name)

    def new_assembly(self, name):
        self._add({"do": "new_assembly", "name": name})
        return Assembly(self, name)

    def part(self, name):
        """Деталь, которая уже есть в проекте."""
        return Part(self, name)

    def assembly(self, name):
        """Сборка, которая уже есть в проекте."""
        return Assembly(self, name)

    def fastener(self, kind, d, length=0):
        """Крепёж из библиотеки: kind — "bolt", "nut" или "washer". Возвращает имя детали."""
        return self._ask({"do": "fastener", "kind": kind, "d": d, "length": length})["name"]

    def angle(self, b, t, length):
        """Отрезок равнополочного уголка ГОСТ 8509-93: ширина полки, толщина, длина. Возвращает имя детали."""
        return self._ask({"do": "angle", "b": b, "t": t, "length": length})["name"]

    def channel(self, name, length):
        """Отрезок швеллера ГОСТ 8240-97 по номеру профиля, например "16П". Возвращает имя детали."""
        return self._ask({"do": "channel", "name": name, "length": length})["name"]

    def beam(self, name, length):
        """Отрезок двутавра ГОСТ Р 57837-2017 по номеру профиля, например "20Б1". Возвращает имя детали."""
        return self._ask({"do": "beam", "name": name, "length": length})["name"]

    def pipe(self, d, length, sdr=None, wall=None, material="PP-R"):
        """Отрезок полипропиленовой трубы как деталь: диаметр, длина и серия SDR (или толщина стенки). Возвращает имя детали."""
        return self._ask(_pipe({"do": "pipe", "material": material, "d": d, "length": length}, sdr, wall))["name"]

    def pipe_check(self, d, pressure, service_class, sdr=None, wall=None, material="PP-R"):
        """Расчёт трубы под давлением (МПа) для класса эксплуатации "1", "2", "4", "5" или "ХВ".

        Возвращает словарь: fits (подходит ли), stress_mpa, design_stress_mpa, allowed_pressure_mpa, minimum_wall_mm,
        life_years, sdr, wall, report (строки отчёта)."""
        return self._ask(_pipe({"do": "pipe_check", "material": material, "d": d, "pressure": pressure, "class": service_class}, sdr, wall))

    def documents(self):
        """Список документов проекта: [{"name": ..., "kind": "part" | "assembly" | "drawing"}, ...]."""
        return self._ask({"do": "documents"})["documents"]

    def export_step(self, document, path):
        self._add({"do": "export_step", "document": document, "path": str(path)})

    def export_stl(self, document, path):
        self._add({"do": "export_stl", "document": document, "path": str(path)})

    # ---- выполнение ----

    def run(self):
        """Выполняет накопленные команды и сохраняет проект. Возвращает ответы программы."""
        if not self._pending:
            return []
        commands, self._pending = self._pending, []
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as handle:
            json.dump(commands, handle, ensure_ascii=False)
            script = handle.name
        try:
            result = subprocess.run([self._cli, "batch", self.path, script], capture_output=True, text=True, encoding="utf-8")
        finally:
            os.unlink(script)
        if result.returncode != 0:
            raise CosmoCadError((result.stderr or result.stdout).strip())
        return json.loads(result.stdout)

    def _add(self, command):
        self._pending.append(command)

    def _ask(self, command):
        self._add(command)
        return self.run()[-1]

    def _name(self, prefix):
        self._names += 1
        return "%s%d" % (prefix, self._names)


class Part:
    """Деталь: параметры, эскизы, операции построения."""

    def __init__(self, project, name):
        self.project = project
        self.name = name

    def parameter(self, name, value):
        """Создаёт параметр или меняет его значение. value — число или формула."""
        self.project._add({"do": "parameter", "part": self.name, "name": name, "value": value})

    def sketch(self, shapes, plane="XY", offset=0, face=None):
        """Эскиз на плоскости XY, XZ, YZ или на плоской грани тела (face — имя грани). Возвращает имя эскиза."""
        alias = self.project._name("эскиз")
        command = {"do": "sketch", "part": self.name, "shapes": shapes, "plane": plane, "offset": offset, "as": alias}
        if face:
            command["face"] = face
        self.project._add(command)
        return alias

    def extrude(self, sketch, distance, cut=False, reversed=False):
        self.project._add({"do": "extrude", "part": self.name, "sketch": sketch, "distance": distance,
                           "cut": cut, "reversed": reversed})

    def revolve(self, sketch, angle=360, axis="Y", cut=False):
        self.project._add({"do": "revolve", "part": self.name, "sketch": sketch, "angle": angle, "axis": axis, "cut": cut})

    def sweep(self, sketch, path, radius=0, cut=False):
        """Протяжка: path — точки траектории после начальной, [(x, y, z), ...]; radius — радиус гиба."""
        self.project._add({"do": "sweep", "part": self.name, "sketch": sketch, "path": [list(p) for p in path],
                           "radius": radius, "cut": cut})

    def fillet(self, edges, size):
        """Скругление рёбер: edges — пары имён граней, [("E1/end", "E2/side/1000"), ...]."""
        self.project._add({"do": "fillet", "part": self.name, "edges": [list(e) for e in edges], "size": size})

    def chamfer(self, edges, size):
        self.project._add({"do": "chamfer", "part": self.name, "edges": [list(e) for e in edges], "size": size})

    def mirror(self, plane="YZ", offset=0):
        self.project._add({"do": "mirror", "part": self.name, "plane": plane, "offset": offset})

    def material(self, title):
        """Материал из библиотеки: «Сталь 45 ГОСТ 1050-2013»."""
        self.project._add({"do": "material", "part": self.name, "title": title})

    def designation(self, value):
        self.project._add({"do": "designation", "part": self.name, "value": value})

    def save_configuration(self, name, designation=""):
        self.project._add({"do": "save_configuration", "part": self.name, "name": name, "designation": designation})

    def apply_configuration(self, name):
        self.project._add({"do": "apply_configuration", "part": self.name, "name": name})

    def info(self):
        """Объём (мм³), масса (г), имена граней, значения параметров, сбои построения."""
        return self.project._ask({"do": "info", "document": self.name})


class Assembly:
    """Сборка: компоненты и сопряжения."""

    def __init__(self, project, name):
        self.project = project
        self.name = name

    def add(self, source, at=(0, 0, 0)):
        """Вставляет деталь или другую сборку по имени документа. Возвращает имя компонента."""
        alias = self.project._name("компонент")
        self.project._add({"do": "component", "assembly": self.name, "source": source, "at": list(at), "as": alias})
        return alias

    def mate(self, kind, first, second, value=0, flip=False):
        """Сопряжение: kind — coincident, concentric, parallel, distance, angle, tangent;
        first и second — пары (имя компонента, имя грани)."""
        self.project._add({"do": "mate", "assembly": self.name, "kind": kind, "first": list(first),
                           "second": list(second), "value": value, "flip": flip})

    def info(self):
        """Состав изделия и число пересечений."""
        return self.project._ask({"do": "info", "document": self.name})
