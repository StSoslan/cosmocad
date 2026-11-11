#!/usr/bin/env python3
"""Проверка требований ОБ-5 и ОБ-6: границы между модулями.

Каждому модулю разрешено подключать заголовки только из перечисленных модулей.
Qt разрешён только в ui и apps/cosmocad; заголовки OCCT — только в geometry/adapters.
Запускается при каждой сборке (тест module_boundaries).

Использование: check_module_boundaries.py <корень репозитория>
"""
import pathlib
import re
import sys

# Модуль (каталог) → какие пространства заголовков cosmocad/<...>/ ему доступны.
ALLOWED = {
    "core": {"core", "document"},          # core/ содержит Core и Document
    "geometry": {"core", "geometry"},
    "drawing": {"core", "document", "geometry", "drawing"},
    "standards": {"core", "document", "geometry", "drawing", "standards"},
    "graphics": {"core", "document", "geometry", "drawing", "standards", "graphics"},
    "exchange": {"core", "document", "geometry", "drawing", "standards", "graphics", "exchange"},
    "storage": {"core", "document", "storage"},
    "modeling": {"core", "document", "geometry", "modeling"},
    "assembly": {"core", "document", "geometry", "assembly"},
    "schematic": {"core", "document", "geometry", "drawing", "schematic"},
    "sketcher": {"core", "document", "geometry", "drawing", "sketcher"},
    "libraries": {"core", "document", "geometry", "modeling", "libraries"},
    "piping": {"core", "document", "geometry", "modeling", "piping"},
}
# Внутри core/ подкаталог core не должен знать о документах.
CORE_ONLY = ("core/core", "core/events")

QT_ALLOWED_PREFIXES = ("ui/", "apps/cosmocad/")
OCCT_ALLOWED_PREFIXES = ("geometry/adapters/",)

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
QT_HEADER = re.compile(r"^(Q[A-Z]\w*|Qt\w*/.*|q\w+\.h)$")
OCCT_HEADER = re.compile(r"^(BRep|TopoDS|TopExp|TopTools|TopLoc|Geom|GC_|GCPnts|GProp|gp_|Standard_|NCollection|Poly_|HLR|Message|TDF|TNaming|TDocStd|AIS_|V3d_|STEPControl|IGESControl|StlAPI).*\.hxx$")
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp"}
SKIP = {".git", "build", "third_party", "tests"}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    problems = []
    checked = 0

    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(root).as_posix()
        if relative.split("/")[0] in SKIP:
            continue
        module = relative.split("/")[0]
        checked += 1
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            match = INCLUDE.match(line)
            if not match:
                continue
            header = match.group(1)
            where = f"{relative}:{number}"
            if QT_HEADER.match(header) and not relative.startswith(QT_ALLOWED_PREFIXES):
                problems.append(f"{where}: Qt разрешён только в ui и apps/cosmocad ({header})")
            if OCCT_HEADER.match(header) and not relative.startswith(OCCT_ALLOWED_PREFIXES):
                problems.append(f"{where}: заголовки OCCT разрешены только в geometry/adapters ({header})")
            if header.startswith("cosmocad/"):
                target = header.split("/")[1]
                if module in ALLOWED and target not in ALLOWED[module]:
                    problems.append(f"{where}: модуль «{module}» не должен зависеть от «{target}» ({header})")
                if relative.startswith(CORE_ONLY) and target != "core":
                    problems.append(f"{where}: базовые типы не должны зависеть от «{target}» ({header})")

    if problems:
        print("Нарушены границы модулей (ОБ-5, ОБ-6):")
        for problem in problems:
            print("  " + problem)
        return 1
    print(f"ОБ-5, ОБ-6: границы модулей соблюдены (проверено файлов: {checked}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
