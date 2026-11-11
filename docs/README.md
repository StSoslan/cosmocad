# Проектные документы CosmoCad

Десять результатов, которых требует раздел 14 исходного ТЗ, и где они лежат.

| № | Результат по ТЗ | Где | Состояние |
| --- | --- | --- | --- |
| 1 | Системное техническое задание | Отдельный документ «CosmoCad — системное техническое задание»; перечень требований с отметками выполнения — [requirements-status.md](requirements-status.md) | готово |
| 2 | Архитектурное описание и диаграммы | [architecture/overview.md](architecture/overview.md), семь схем в [uml/](uml/) | готово |
| 3 | Каталог модулей и границ ответственности | [architecture/modules.md](architecture/modules.md) | готово |
| 4 | Спецификации интерфейсов | [api/interfaces.md](api/interfaces.md) и заголовочные файлы в коде | готово |
| 5 | Модель данных и формат файла | [architecture/data-format.md](architecture/data-format.md) | готово для версии формата 1 |
| 6 | Структура репозитория | [architecture/repository.md](architecture/repository.md) | готово |
| 7 | План разработки с зависимостями и критериями готовности | [development-plan.md](development-plan.md) | готово |
| 8 | План тестирования | [test-plan.md](test-plan.md) | готово |
| 9 | Риски, ограничения и альтернативы | [risks.md](risks.md) | готово |
| 10 | Поэтапные исходные реализации модулей | Код репозитория: этапы 1 и 3 целиком, этапы 2, 4, 5 частично | начато |

## Семь схем (раздел 9 ТЗ)

| № | Схема | Описывает |
| --- | --- | --- |
| 1 | [Компоненты](uml/01-components.md) | готовый код |
| 2 | [Модель документа](uml/02-document-model.md) | готовый код |
| 3 | [Зависимости и перестроение](uml/03-recompute.md) | проект этапа 6 |
| 4 | [Чертёжный вид](uml/04-drawing-view.md) | проект этапа 7 |
| 5 | [Окно, сервисы, ядро](uml/05-ui-services-kernel.md) | готовый код и проект этапа 6 |
| 6 | [Сохранение и восстановление](uml/06-save-recovery.md) | готовый код |
| 7 | [Дополнения](uml/07-plugins.md) | проект этапа 10 |

Схемы написаны на Mermaid: их показывают GitHub, GitLab и большинство редакторов Markdown.
Проверка синтаксиса: `node scripts/check_diagrams.mjs docs/uml`.

## Остальное

- [standards/eskd-coverage.md](standards/eskd-coverage.md) — что из ЕСКД действительно покрыто.
- [exchange/dxf.md](exchange/dxf.md), [exchange/svg.md](exchange/svg.md), [exchange/pdf.md](exchange/pdf.md) — паспорта форматов.
- [licenses.md](licenses.md) — сторонние библиотеки и их лицензии.
- [user_manual/quick-start.md](user_manual/quick-start.md) — как пользоваться программой.

## Правило

Документы описывают то, что есть в коде, и меняются вместе с ним. Там, где описан замысел,
а не готовый код, это сказано в первой строке.
