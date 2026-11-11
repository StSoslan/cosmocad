#include "cosmocad/document/Document.h"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>

#include "cosmocad/document/IDocumentService.h"

namespace cosmocad::document {

using core::ErrorCode;
using core::makeError;
using nlohmann::json;

// ---- PropertyValue ----

json propertyToJson(const PropertyValue& value) {
    return std::visit(
        [](const auto& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Uuid>) return json{{"$ref", v.toString()}};
            else return json(v);
        },
        value);
}

Result<PropertyValue> propertyFromJson(const json& j) {
    if (j.is_boolean()) return PropertyValue{j.get<bool>()};
    if (j.is_number_integer()) return PropertyValue{j.get<std::int64_t>()};
    if (j.is_number()) return PropertyValue{j.get<double>()};
    if (j.is_string()) return PropertyValue{j.get<std::string>()};
    if (j.is_object() && j.contains("$ref") && j["$ref"].is_string()) {
        auto id = Uuid::parse(j["$ref"].get<std::string>());
        if (!id) return id.error();
        return PropertyValue{id.value()};
    }
    return makeError(ErrorCode::ParseError, "неподдерживаемый тип значения свойства");
}

// ---- Object ----

std::unique_ptr<Object> Object::duplicate() const {
    auto copy = clone();
    copy->id_ = Uuid::generate();
    return copy;
}

json Object::toJson() const {
    json j;
    j["type"] = std::string(typeName());
    j["id"] = id_.toString();
    if (!name.empty()) j["name"] = name;
    if (!attributes.empty()) {
        json attrs = json::object();
        for (const auto& [key, value] : attributes) attrs[key] = propertyToJson(value);
        j["attributes"] = std::move(attrs);
    }
    writeJson(j);
    return j;
}

Result<void> Object::fromJson(const json& j) {
    try {
        auto id = Uuid::parse(j.at("id").get<std::string>());
        if (!id) return id.error();
        id_ = id.value();
        name = j.value("name", std::string());
        attributes.clear();
        if (j.contains("attributes")) {
            for (const auto& [key, value] : j.at("attributes").items()) {
                auto parsed = propertyFromJson(value);
                if (!parsed) return parsed.error();
                attributes[key] = parsed.value();
            }
        }
        return readJson(j);
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("объект: ") + e.what());
    }
}

// ---- ObjectFactory ----

void ObjectFactory::registerType(std::string_view typeName, Creator creator) {
    creators_[std::string(typeName)] = std::move(creator);
}

bool ObjectFactory::knows(std::string_view typeName) const {
    return creators_.count(std::string(typeName)) > 0;
}

Result<std::unique_ptr<Object>> ObjectFactory::create(const json& j) const {
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string())
        return makeError(ErrorCode::ParseError, "у объекта не указан тип");
    auto it = creators_.find(j["type"].get<std::string>());
    if (it == creators_.end()) {
        // Тип из дополнения, которого сейчас нет: объект сохраняется как есть.
        std::unique_ptr<Object> unknown = std::make_unique<UnknownObject>();
        if (auto read = unknown->fromJson(j); !read) return read.error();
        return unknown;
    }
    auto object = it->second();
    if (auto read = object->fromJson(j); !read) return read.error();
    return object;
}

void UnknownObject::writeJson(json& j) const {
    for (const auto& [key, value] : raw_.items())
        if (!j.contains(key)) j[key] = value;
}

Result<void> UnknownObject::readJson(const json& j) {
    typeName_ = j.at("type").get<std::string>();
    raw_ = j;
    return {};
}

// ---- DocumentKind ----

const char* toString(DocumentKind kind) {
    switch (kind) {
        case DocumentKind::Drawing: return "Drawing";
        case DocumentKind::Sketch: return "Sketch";
        case DocumentKind::Part: return "Part";
        case DocumentKind::Assembly: return "Assembly";
        case DocumentKind::Schematic: return "Schematic";
    }
    return "Drawing";
}

Result<DocumentKind> documentKindFromString(std::string_view text) {
    for (auto kind : {DocumentKind::Drawing, DocumentKind::Sketch, DocumentKind::Part, DocumentKind::Assembly,
                      DocumentKind::Schematic})
        if (text == toString(kind)) return kind;
    return makeError(ErrorCode::ParseError, "неизвестный вид документа: " + std::string(text));
}

// ---- Document ----

Document::Document(DocumentKind kind, std::string documentName)
    : name(std::move(documentName)), id_(Uuid::generate()), kind_(kind) {}

const Object* Document::find(const Uuid& id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

Object* Document::findMutable(const Uuid& id) {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

std::size_t Document::indexOf(const Uuid& id, std::size_t hint) const {
    if (hint < objects_.size() && objects_[hint]->id() == id) return hint;
    if (index_.find(id) == index_.end()) return objects_.size();
    for (std::size_t i = 0; i < objects_.size(); ++i)
        if (objects_[i]->id() == id) return i;
    return objects_.size();
}

void Document::insertAt(std::size_t index, std::unique_ptr<Object> object) {
    index = std::min(index, objects_.size());
    index_[object->id()] = object.get();
    objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(index), std::move(object));
}

std::unique_ptr<Object> Document::removeById(const Uuid& id, std::size_t hint) {
    auto position = indexOf(id, hint);
    if (position == objects_.size()) return nullptr;
    auto object = std::move(objects_[position]);
    objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(position));
    index_.erase(id);
    return object;
}

void Document::applyForward(const Change& change) {
    if (!change.before) {
        insertAt(change.index, change.after->clone());
    } else if (!change.after) {
        removeById(change.id, change.index);
    } else {
        auto position = indexOf(change.id, change.index);
        removeById(change.id, position);
        insertAt(position, change.after->clone());
    }
}

void Document::applyBackward(const Change& change) {
    if (!change.before) {
        removeById(change.id, change.index);
    } else if (!change.after) {
        insertAt(change.index, change.before->clone());
    } else {
        auto position = indexOf(change.id, change.index);
        removeById(change.id, position);
        insertAt(position, change.before->clone());
    }
}

void Document::addJournal(JournalEntry::Kind kind, const std::string& entryName, std::size_t count) {
    using namespace std::chrono;
    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    journal_.push_back(JournalEntry{kind, entryName, static_cast<std::int64_t>(now), count});
}

void Document::pushCommitted(ChangeSet set) {
    addJournal(JournalEntry::Kind::Commit, set.name, set.changes.size());
    undoStack_.push_back(std::move(set));
    redoStack_.clear();
    modified_ = true;
    changed.notify();
}

bool Document::undo() {
    if (undoStack_.empty() || transactionActive_) return false;
    ChangeSet set = std::move(undoStack_.back());
    undoStack_.pop_back();
    for (auto it = set.changes.rbegin(); it != set.changes.rend(); ++it) applyBackward(*it);
    addJournal(JournalEntry::Kind::Undo, set.name, set.changes.size());
    redoStack_.push_back(std::move(set));
    modified_ = true;
    changed.notify();
    return true;
}

bool Document::redo() {
    if (redoStack_.empty() || transactionActive_) return false;
    ChangeSet set = std::move(redoStack_.back());
    redoStack_.pop_back();
    for (const auto& change : set.changes) applyForward(change);
    addJournal(JournalEntry::Kind::Redo, set.name, set.changes.size());
    undoStack_.push_back(std::move(set));
    modified_ = true;
    changed.notify();
    return true;
}

json Document::toJson() const {
    json j;
    j["id"] = id_.toString();
    j["kind"] = toString(kind_);
    j["name"] = name;
    j["profileId"] = profileId;
    j["profileVersion"] = profileVersion;
    json objects = json::array();
    for (const auto& object : objects_) objects.push_back(object->toJson());
    j["objects"] = std::move(objects);
    return j;
}

Result<std::unique_ptr<Document>> Document::fromJson(const json& j, const ObjectFactory& factory) {
    try {
        auto kind = documentKindFromString(j.at("kind").get<std::string>());
        if (!kind) return kind.error();
        auto id = Uuid::parse(j.at("id").get<std::string>());
        if (!id) return id.error();
        auto document = std::make_unique<Document>(kind.value(), j.value("name", std::string()));
        document->id_ = id.value();
        document->profileId = j.value("profileId", std::string());
        document->profileVersion = j.value("profileVersion", std::string());
        for (const auto& item : j.at("objects")) {
            auto object = factory.create(item);
            if (!object) return object.error();
            if (document->find(object.value()->id()))
                return makeError(ErrorCode::CorruptedData, "в документе повторяется номер объекта");
            document->insertAt(document->objects_.size(), object.take());
        }
        return document;
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("документ: ") + e.what());
    }
}

std::vector<Uuid> Document::findDanglingReferences() const {
    std::vector<Uuid> dangling;
    for (const auto& object : objects_)
        for (const auto& [key, value] : object->attributes)
            if (auto* ref = std::get_if<Uuid>(&value); ref && !ref->isNil() && !find(*ref))
                dangling.push_back(object->id());
    return dangling;
}

// ---- Project ----

Project::Project(std::string projectName) : name(std::move(projectName)), id_(Uuid::generate()) {}

Document& Project::addDocument(std::unique_ptr<Document> document) {
    documents_.push_back(std::move(document));
    return *documents_.back();
}

Result<void> Project::removeDocument(const Uuid& id) {
    auto it = std::find_if(documents_.begin(), documents_.end(), [&](const auto& d) { return d->id() == id; });
    if (it == documents_.end()) return makeError(ErrorCode::NotFound, "документ не найден");
    documents_.erase(it);
    return {};
}

Document* Project::findDocument(const Uuid& id) {
    for (auto& document : documents_)
        if (document->id() == id) return document.get();
    return nullptr;
}

// ---- DocumentService ----

DocumentService::DocumentService(IProjectStorage& storage)
    : storage_(storage), project_(std::make_unique<Project>("Без имени")) {}

void DocumentService::newProject(std::string projectName) {
    project_ = std::make_unique<Project>(std::move(projectName));
}

Result<void> DocumentService::openProject(const std::filesystem::path& path) {
    auto loaded = storage_.load(path);
    if (!loaded) return loaded.error();
    project_ = loaded.take();
    return {};
}

Result<void> DocumentService::saveProject(const std::filesystem::path& path) {
    auto saved = storage_.save(*project_, path);
    if (!saved) return saved;
    for (const auto& document : project_->documents()) document->markSaved();
    return {};
}

Result<Document*> DocumentService::createDocument(DocumentKind kind, std::string documentName) {
    if (documentName.empty()) return makeError(ErrorCode::InvalidArgument, "имя документа не задано");
    return &project_->addDocument(std::make_unique<Document>(kind, std::move(documentName)));
}

Result<void> DocumentService::removeDocument(const Uuid& id) { return project_->removeDocument(id); }

Document* DocumentService::findDocument(const Uuid& id) { return project_->findDocument(id); }

}  // namespace cosmocad::document
