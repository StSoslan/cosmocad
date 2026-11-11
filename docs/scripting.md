# Скрипты на Python

Скрипт — обычная программа на Python, которая подключает модуль `cosmocad` и строит модель.
Модуль лежит в `scripting/python/cosmocad.py`, ему нужна только стандартная библиотека Python.
Библиотек ИИ нет и не требуется.

## Как запустить

```
COSMOCAD_CLI=/путь/к/cosmocad-cli python3 мой_скрипт.py
```

Программу `cosmocad-cli` модуль ищет в таком порядке: `Project(..., cli="путь")`, переменная
окружения `COSMOCAD_CLI`, затем обычный поиск по `PATH`.

## Пример

```python
import cosmocad

with cosmocad.Project("плита.cosmocad") as project:
    plate = project.new_part("Плита")
    plate.parameter("длина", 120)
    plate.parameter("толщина", 12)
    base = plate.sketch([cosmocad.rectangle(0, 0, "длина", 80),
                         cosmocad.circle(15, 15, 4.5, hole=True)])
    plate.extrude(base, "толщина")
    plate.material("Сталь 45 ГОСТ 1050-2013")
    print(plate.info()["mass_g"], "г")

    bolt = project.fastener("bolt", 8, 30)        # «Болт М8×30 ГОСТ 7798-70»
    unit = project.new_assembly("Узел")
    unit.add("Плита")
    unit.add(bolt, at=(15, 15, 12))
    project.export_step("Узел", "узел.step")
```

Размеры можно задавать числами или формулами-строками (`"длина - 15"`). Команды выполняются, когда
нужен ответ (`info`, `fastener`, `documents`) и при выходе из блока `with`. Если программа отказала,
возникает ошибка `cosmocad.CosmoCadError` с объяснением, а файл проекта остаётся прежним.

Полный пример — `scripting/python/examples/plate_with_bolts.py`.

## Что умеет модуль

| Объект | Действия |
| --- | --- |
| `Project` | `new_part`, `new_assembly`, `part`, `assembly`, `fastener`, `angle`, `channel`, `beam`, `pipe`, `pipe_check`, `documents`, `export_step`, `export_stl`, `run` |
| `Part` | `parameter`, `sketch`, `extrude`, `revolve`, `sweep`, `fillet`, `chamfer`, `mirror`, `material`, `designation`, `save_configuration`, `apply_configuration`, `info` |
| `Assembly` | `add`, `mate`, `info` |
| Фигуры эскиза | `cosmocad.rectangle`, `cosmocad.circle`, `cosmocad.polygon` |

`Part.info()` возвращает объём, массу, имена граней, значения параметров и список сбоев построения.
Имена граней нужны для скруглений, фасок, эскизов на грани и сопряжений.

## Пакетный режим без Python

Модуль — тонкая оболочка над командой

```
cosmocad-cli batch проект.cosmocad команды.json
```

Файл команд — массив JSON. У каждой команды есть поле `do` и свои поля:

| `do` | Поля |
| --- | --- |
| `new_part`, `new_assembly` | `name` |
| `parameter` | `part`, `name`, `value` |
| `sketch` | `part`, `shapes` (`kind`: rectangle, circle, polygon; `values`; `hole`), `plane` или `face`, `offset`, `as` |
| `extrude` | `part`, `sketch`, `distance`, `cut`, `reversed` |
| `revolve` | `part`, `sketch`, `angle`, `axis`, `cut` |
| `sweep` | `part`, `sketch`, `path` (точки `[x, y, z]`), `radius`, `cut` |
| `fillet`, `chamfer` | `part`, `edges` (пары имён граней), `size` |
| `mirror` | `part`, `plane`, `offset` |
| `material` | `part`, `title` |
| `designation` | `part`, `value` |
| `save_configuration`, `apply_configuration` | `part`, `name`, `designation` |
| `fastener` | `kind` (bolt, nut, washer), `d`, `length` |
| `angle` | `b`, `t`, `length` — отрезок уголка ГОСТ 8509-93 |
| `channel` | `name` (например «16П»), `length` — отрезок швеллера ГОСТ 8240-97 |
| `beam` | `name` (например «20Б1»), `length` — отрезок двутавра ГОСТ Р 57837-2017 |
| `pipe` | `material`, `d`, `sdr` или `wall`, `length` — отрезок полипропиленовой трубы |
| `pipe_check` | `material`, `class`, `d`, `sdr` или `wall`, `pressure` (МПа) — расчёт трубы |
| `component` | `assembly`, `source`, `at`, `as` |
| `mate` | `assembly`, `kind`, `first` и `second` (пары `[компонент, грань]`), `value`, `flip` |
| `info` | `document` |
| `export_step`, `export_stl` | `document`, `path` |
| `documents` | — |

Программа печатает массив ответов, по одному на команду. Первая же ошибка останавливает выполнение:
в сообщении названы номер команды и причина, проект не сохраняется.

## Чего нет

- Скрипт работает отдельно от окна программы: открытое окно его изменений не увидит, пока проект не открыть заново.
- Чертежи, схемы и ограничения эскиза из скрипта недоступны.
- Встроенного в программу окна для скриптов нет.
