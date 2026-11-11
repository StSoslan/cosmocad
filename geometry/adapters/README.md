# Адаптер OCCT

`occt/` — реализация `IGeometryKernel` на OpenCASCADE 7.6. Единственное место проекта, где подключаются заголовки OCCT (проверяет `scripts/check_module_boundaries.py`). Здесь же ведутся устойчивые имена граней. Тесты — `Kernel3D.*`.
