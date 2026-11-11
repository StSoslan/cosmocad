// Документ — набор объектов с историей изменений.
// Менять объекты можно только через Transaction (см. Transaction.h): это гарантирует,
// что любое действие отменяется целиком и точно.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cosmocad/core/Signal.h"
#include "cosmocad/document/Object.h"

namespace cosmocad::document {

enum class DocumentKind { Drawing, Sketch, Part, Assembly, Schematic };

const char* toString(DocumentKind kind);
Result<DocumentKind> documentKindFromString(std::string_view text);

struct Change {
    Uuid id;
    std::unique_ptr<Object> before;  // пусто — объект добавлен
    std::unique_ptr<Object> after;   // пусто — объект удалён
    std::size_t index = 0;           // место объекта в порядке документа
};

struct ChangeSet {
    std::string name;
    std::vector<Change> changes;
};

struct JournalEntry {
    enum class Kind { Commit, Undo, Redo };
    Kind kind = Kind::Commit;
    std::string name;
    std::int64_t timestampMs = 0;
    std::size_t changeCount = 0;
};

class Document {
public:
    Document(DocumentKind kind, std::string name);

    const Uuid& id() const { return id_; }
    DocumentKind kind() const { return kind_; }

    // Сведения о документе, не входящие в историю отмены.
    std::string name;
    std::string profileId;       // профиль стандарта, по которому оформлен документ
    std::string profileVersion;  // версия профиля на момент создания

    // --- Чтение ---
    std::size_t size() const { return objects_.size(); }
    const Object* find(const Uuid& id) const;
    template <class T>
    const T* findAs(const Uuid& id) const {
        return dynamic_cast<const T*>(find(id));
    }
    const std::vector<std::unique_ptr<Object>>& objects() const { return objects_; }
    template <class T>
    std::vector<const T*> objectsOf() const {
        std::vector<const T*> out;
        for (const auto& object : objects_)
            if (auto* typed = dynamic_cast<const T*>(object.get())) out.push_back(typed);
        return out;
    }

    // --- Отмена и повтор ---
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    std::string undoName() const { return canUndo() ? undoStack_.back().name : std::string(); }
    std::string redoName() const { return canRedo() ? redoStack_.back().name : std::string(); }
    bool undo();
    bool redo();

    const std::vector<JournalEntry>& journal() const { return journal_; }
    bool isModified() const { return modified_; }
    void markSaved() { modified_ = false; }

    core::Signal<> changed;  // вызывается после фиксации, отмены и повтора

    // --- Запись в файл и чтение из файла ---
    nlohmann::json toJson() const;
    static Result<std::unique_ptr<Document>> fromJson(const nlohmann::json& json, const ObjectFactory& factory);

    // Проверка ссылочной целостности: ссылки (Uuid) в атрибутах указывают на существующие объекты.
    std::vector<Uuid> findDanglingReferences() const;

private:
    friend class Transaction;

    void applyForward(const Change& change);
    void applyBackward(const Change& change);
    void insertAt(std::size_t index, std::unique_ptr<Object> object);
    std::unique_ptr<Object> removeById(const Uuid& id, std::size_t hint = static_cast<std::size_t>(-1));
    Object* findMutable(const Uuid& id);
    // hint — предполагаемое место объекта; при отмене и повторе оно точное, и поиск не нужен.
    std::size_t indexOf(const Uuid& id, std::size_t hint = static_cast<std::size_t>(-1)) const;
    void pushCommitted(ChangeSet set);
    void addJournal(JournalEntry::Kind kind, const std::string& name, std::size_t count);

    Uuid id_;
    DocumentKind kind_;
    std::vector<std::unique_ptr<Object>> objects_;
    std::unordered_map<Uuid, Object*> index_;
    std::vector<ChangeSet> undoStack_;
    std::vector<ChangeSet> redoStack_;
    std::vector<JournalEntry> journal_;
    bool transactionActive_ = false;
    bool modified_ = false;
};

class Project {
public:
    explicit Project(std::string name);

    const Uuid& id() const { return id_; }
    std::string name;

    Document& addDocument(std::unique_ptr<Document> document);
    Result<void> removeDocument(const Uuid& id);
    Document* findDocument(const Uuid& id);
    const std::vector<std::unique_ptr<Document>>& documents() const { return documents_; }

    // Используется хранилищем при загрузке, чтобы сохранить номер проекта.
    void restoreId(const Uuid& id) { id_ = id; }

private:
    Uuid id_;
    std::vector<std::unique_ptr<Document>> documents_;
};

}  // namespace cosmocad::document
