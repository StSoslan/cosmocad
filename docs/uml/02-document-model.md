# Схема 2. Модель документа

Что лежит в проекте и как это меняется. Схема соответствует коду в `core/document`,
`core/transactions` и `drawing`.

```mermaid
classDiagram
    class Project {
        +Uuid id
        +string name
        +addDocument()
        +removeDocument()
        +findDocument()
    }
    class Document {
        +Uuid id
        +DocumentKind kind
        +string name
        +string profileId
        +string profileVersion
        +find(id)
        +objectsOf~T~()
        +undo()
        +redo()
        +journal()
        +toJson()
        +fromJson()
    }
    class Object {
        <<abstract>>
        +Uuid id
        +string name
        +map attributes
        +typeName()
        +clone()
        +toJson()
    }
    class Transaction {
        +add(object)
        +remove(id)
        +modify(id, change)
        +commit()
        +rollback()
    }
    class ChangeSet {
        +string name
        +Change[] changes
    }
    class Change {
        +Uuid id
        +Object before
        +Object after
        +index
    }
    class JournalEntry {
        +kind
        +string name
        +timestamp
        +changeCount
    }
    class ObjectFactory {
        +registerType()
        +create(json)
    }
    class Layer {
        +bool visible
        +bool locked
        +string color
        +string lineTypeId
        +double lineWidth
    }
    class Sheet {
        +string format
        +bool landscape
        +double mainLineWidth
        +map titleBlock
    }
    class Entity {
        <<abstract>>
        +Uuid layerId
        +lineTypeId
        +lineWidth
        +bounds()
    }
    class LineEntity {
        +Point2 start
        +Point2 end
    }
    class CircleEntity {
        +Point2 center
        +double radius
    }
    class ArcEntity {
        +Point2 center
        +double radius
        +double startAngle
        +double endAngle
    }
    class TextEntity {
        +Point2 position
        +string text
        +double height
    }
    class LinearDimension {
        +Point2 p1
        +Point2 p2
        +double offset
        +double textHeight
    }

    Project "1" *-- "0..*" Document
    Document "1" *-- "0..*" Object
    Document "1" *-- "0..*" ChangeSet : история отмены
    Document "1" *-- "0..*" JournalEntry : журнал
    ChangeSet "1" *-- "1..*" Change
    Transaction ..> Document : изменяет
    Transaction ..> ChangeSet : создаёт
    ObjectFactory ..> Object : создаёт при чтении файла
    Object <|-- Layer
    Object <|-- Sheet
    Object <|-- Entity
    Entity <|-- LineEntity
    Entity <|-- CircleEntity
    Entity <|-- ArcEntity
    Entity <|-- TextEntity
    Entity <|-- LinearDimension
    Entity ..> Layer : layerId
```

## Главные правила

1. **У каждого объекта постоянный номер** (`Uuid`). Он не меняется при сохранении, загрузке,
   отмене и повторе.
2. **Слой и лист — такие же объекты**, как линии. Поэтому их изменение отменяется тем же
   механизмом, что и изменение геометрии.
3. **Изменить документ можно только через `Transaction`.** Транзакция запоминает для каждого
   затронутого объекта состояние «до» и «после». Незавершённая транзакция откатывается сама.
4. **Отмена — это применение сохранённых изменений в обратном порядке**, а не «обратная команда».
   Поэтому отмена всегда точна: тест выполняет 1000 случайных команд, отменяет их и сравнивает
   документ с исходным.
5. **Виды документов** перечислены заранее: чертёж, эскиз, деталь, сборка, схема. В версии 0.1
   объекты определены только для чертежа.
