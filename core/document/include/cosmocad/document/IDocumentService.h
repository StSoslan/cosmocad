// IDocumentService — создание, загрузка, сохранение и управление документами.
// Полное описание договорённостей: docs/api/interfaces.md.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "cosmocad/document/Document.h"

namespace cosmocad::document {

inline constexpr int kDocumentServiceApiVersion = 1;

// Хранилище проекта. Реализуется модулем Storage; Document о формате файла не знает.
class IProjectStorage {
public:
    virtual ~IProjectStorage() = default;
    virtual Result<void> save(const Project& project, const std::filesystem::path& path) = 0;
    virtual Result<std::unique_ptr<Project>> load(const std::filesystem::path& path) = 0;
};

class IDocumentService {
public:
    virtual ~IDocumentService() = default;

    virtual Project& project() = 0;
    virtual void newProject(std::string name) = 0;
    virtual Result<void> openProject(const std::filesystem::path& path) = 0;
    virtual Result<void> saveProject(const std::filesystem::path& path) = 0;

    virtual Result<Document*> createDocument(DocumentKind kind, std::string name) = 0;
    virtual Result<void> removeDocument(const Uuid& id) = 0;
    virtual Document* findDocument(const Uuid& id) = 0;
};

class DocumentService final : public IDocumentService {
public:
    explicit DocumentService(IProjectStorage& storage);

    Project& project() override { return *project_; }
    void newProject(std::string name) override;
    Result<void> openProject(const std::filesystem::path& path) override;
    Result<void> saveProject(const std::filesystem::path& path) override;

    Result<Document*> createDocument(DocumentKind kind, std::string name) override;
    Result<void> removeDocument(const Uuid& id) override;
    Document* findDocument(const Uuid& id) override;

private:
    IProjectStorage& storage_;
    std::unique_ptr<Project> project_;
};

}  // namespace cosmocad::document
