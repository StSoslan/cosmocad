# Схема 1. Из каких частей состоит CosmoCad

Стрелка означает «пользуется». Стрелки идут только вниз: нижние части ничего не знают о верхних.
Сплошная рамка — часть есть в коде (на схеме отмечено состояние версии 0.1; в версии 0.2
добавились Geometry-адаптер OCCT, Modeling и Assembly — см. `docs/architecture/modules.md`). Пунктирная — запланирована, кода ещё нет.

Эта схема соответствует файлам `CMakeLists.txt` и проверяется при каждой сборке
программой `scripts/check_module_boundaries.py`. Если меняется код — меняется и схема.

```mermaid
flowchart TD
    subgraph APPS["Приложения"]
        GUI["cosmocad<br/>оконное приложение"]
        CLI["cosmocad-cli<br/>консольная утилита"]
    end

    subgraph UIL["Интерфейс пользователя — единственное место, где есть Qt"]
        UI["UI<br/>окна, виджеты, рисование, PDF"]
    end

    APP["AppContext<br/>связывает модули между собой"]

    subgraph DOMAIN["Предметные модули"]
        EXCH["Exchange<br/>DXF, SVG"]
        GFX["Graphics<br/>сцена, вид, выбор, привязки"]
        STD["Standards<br/>профили стандартов, проверка оформления"]
        DRW["Drawing<br/>объекты и сервис чертежа"]
        STOR["Storage<br/>формат .cosmocad, миграции, восстановление"]
    end

    subgraph BASE["Основа"]
        DOC["Document<br/>документы, транзакции, отмена и повтор"]
        GEO["Geometry<br/>2D-типы, граница с 3D-ядром"]
        CORE["Core<br/>номера объектов, ошибки, журнал, события"]
    end

    subgraph PLANNED["Запланировано"]
        SK["Sketcher<br/>эскизы, решатель"]
        MOD["Modeling<br/>3D-операции, перестроение"]
        ASM["Assembly<br/>сборки"]
        SCH["Schematic<br/>электросхемы"]
        LIB["Libraries<br/>стандартные изделия"]
        PY["Scripting<br/>Python"]
        OCCT["Адаптер OCCT<br/>3D-ядро"]
        EXT["CAE, CAM, PDM<br/>дополнения"]
    end

    GUI --> UI
    GUI --> APP
    CLI --> APP
    CLI --> EXCH
    UI --> APP
    UI --> EXCH
    APP --> STOR
    APP --> STD
    APP --> DRW
    EXCH --> GFX
    GFX --> STD
    STD --> DRW
    DRW --> DOC
    DRW --> GEO
    STOR --> DOC
    DOC --> CORE
    GEO --> CORE

    SK -.-> DOC
    SK -.-> GEO
    MOD -.-> SK
    MOD -.-> OCCT
    OCCT -.-> GEO
    ASM -.-> MOD
    SCH -.-> DRW
    LIB -.-> STOR
    PY -.-> APP
    EXT -.-> APP

    classDef planned stroke-dasharray: 5 5
    class SK,MOD,ASM,SCH,LIB,PY,OCCT,EXT planned
```

## Как читать

- **Core** — самые общие вещи: постоянный номер объекта, результат «значение или ошибка», журнал.
- **Document** — документ как набор объектов. Изменить документ можно только через транзакцию,
  поэтому любое действие отменяется целиком.
- **Geometry** — собственные геометрические типы CosmoCad. Здесь же объявлена граница с 3D-ядром;
  само ядро (OCCT) будет подключено за этой границей и больше нигде.
- **Drawing** — линии, окружности, дуги, текст, размеры, слои, лист.
- **Standards** — профиль стандарта (файл с настройками и правилами) и проверка чертежа по нему.
- **Graphics** — превращает документ в список «что нарисовать». Этим списком пользуются экран,
  PDF, SVG и DXF, поэтому чертёж везде выглядит одинаково.
- **Storage** — запись и чтение проекта.
- **UI** — окна. Правил оформления и геометрии в нём нет.
