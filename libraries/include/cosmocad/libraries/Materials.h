// Библиотека материалов: встроенная (resources/libraries/materials.json) и пользовательская
// (файл в папке пользователя). У каждой записи есть происхождение: откуда взята плотность.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"

namespace cosmocad::libraries {

struct Material {
    std::string name;      // «Сталь 45»
    std::string standard;  // «ГОСТ 1050-2013»; может быть пусто
    double density = 0.0;  // г/см³
    std::string note;      // откуда плотность
    bool user = false;     // запись из пользовательской библиотеки

    // Как материал пишется в основной надписи: «Сталь 45 ГОСТ 1050-2013».
    std::string title() const { return standard.empty() ? name : name + " " + standard; }
};

class MaterialLibrary {
public:
    // builtIn — файл встроенной библиотеки; userFile — файл пользовательской (может не существовать).
    static core::Result<MaterialLibrary> load(const std::filesystem::path& builtIn, const std::filesystem::path& userFile);

    const std::string& version() const { return version_; }
    const std::string& source() const { return source_; }
    const std::filesystem::path& userFile() const { return userFile_; }

    // Все материалы: сначала встроенные, потом пользовательские. Пользовательская запись с тем же
    // названием заменяет встроенную.
    std::vector<Material> all() const;
    // Поиск по части названия или стандарта, без учёта регистра. Пустая строка — все.
    std::vector<Material> find(const std::string& text) const;
    const Material* byTitle(const std::string& title) const;

    // Пользовательская библиотека. Изменения сразу записываются в файл пользователя.
    core::Result<void> addUser(const Material& material);       // такое название уже есть среди своих — замена
    core::Result<void> removeUser(const std::string& title);
    // Обмен: записать свои материалы в файл; добавить материалы из файла к своим. Возвращает число записей.
    core::Result<int> exportUser(const std::filesystem::path& file) const;
    core::Result<int> importUser(const std::filesystem::path& file);

private:
    core::Result<void> saveUser() const;
    std::string version_, source_;
    std::filesystem::path userFile_;
    std::vector<Material> builtIn_, user_;
};

// Строчные буквы для сравнения без учёта регистра (латиница и кириллица).
std::string lowered(const std::string& text);

}  // namespace cosmocad::libraries
