// Пакетный режим: список команд (JSON) выполняется над проектом подряд. Это основа скриптов:
// модуль Python scripting/python/cosmocad.py собирает такой список и передаёт его cosmocad-cli.
//
// Команда — объект с полем "do" и своими полями. Документы называются по именам; эскизы и
// компоненты получают короткие имена через поле "as", по которым на них ссылаются следующие команды.
// Полный список команд — docs/scripting.md.
#pragma once

#include <nlohmann/json.hpp>

#include "AppContext.h"

namespace cosmocad::app {

// Выполняет команды по порядку. Возвращает массив ответов — по одному на команду.
// Первая же ошибка останавливает выполнение: в ошибке назван номер команды и причина.
core::Result<nlohmann::json> runBatch(AppContext& context, const nlohmann::json& commands);

}  // namespace cosmocad::app
