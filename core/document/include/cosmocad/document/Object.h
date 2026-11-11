// Базовый объект модели данных: всё, что лежит в документе (линия, слой, лист),
// является Object с постоянным номером, именем и пользовательскими атрибутами.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include <nlohmann/json.hpp>

#include "cosmocad/core/Result.h"
#include "cosmocad/core/Uuid.h"

namespace cosmocad::document {

using core::Result;
using core::Uuid;

// Типизированное значение свойства. Uuid — ссылка на другой объект.
using PropertyValue = std::variant<bool, std::int64_t, double, std::string, Uuid>;

nlohmann::json propertyToJson(const PropertyValue& value);
Result<PropertyValue> propertyFromJson(const nlohmann::json& json);

class Object {
public:
    virtual ~Object() = default;

    const Uuid& id() const { return id_; }
    virtual std::string_view typeName() const = 0;
    virtual std::unique_ptr<Object> clone() const = 0;  // копия с тем же номером
    std::unique_ptr<Object> duplicate() const;         // копия с новым номером

    std::string name;
    std::map<std::string, PropertyValue> attributes;  // пользовательские атрибуты

    nlohmann::json toJson() const;
    Result<void> fromJson(const nlohmann::json& json);

protected:
    Object() : id_(Uuid::generate()) {}
    Object(const Object&) = default;
    Object& operator=(const Object&) = default;

    virtual void writeJson(nlohmann::json& json) const = 0;
    virtual Result<void> readJson(const nlohmann::json& json) = 0;

private:
    Uuid id_;
};

// Помощник: даёт typeName() и clone() классу-наследнику D с полем `static constexpr kType`.
template <class D, class Base = Object>
class ObjectT : public Base {
public:
    std::string_view typeName() const override { return D::kType; }
    std::unique_ptr<Object> clone() const override {
        return std::make_unique<D>(static_cast<const D&>(*this));
    }
};

// Реестр типов объектов: нужен для чтения документа из файла.
// Объект неизвестного программе типа — например, созданный дополнением, которого сейчас нет.
// Программа его не понимает, но хранит как есть и записывает обратно без потерь, чтобы документ
// можно было открыть, поправить и сохранить, не испортив чужие данные.
class UnknownObject final : public Object {
public:
    std::string_view typeName() const override { return typeName_; }
    std::unique_ptr<Object> clone() const override { return std::unique_ptr<Object>(new UnknownObject(*this)); }
    const nlohmann::json& raw() const { return raw_; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;

private:
    std::string typeName_;
    nlohmann::json raw_;
};

class ObjectFactory {
public:
    using Creator = std::function<std::unique_ptr<Object>()>;

    void registerType(std::string_view typeName, Creator creator);
    template <class T>
    void registerType() {
        registerType(T::kType, [] { return std::make_unique<T>(); });
    }

    bool knows(std::string_view typeName) const;
    Result<std::unique_ptr<Object>> create(const nlohmann::json& json) const;

private:
    std::unordered_map<std::string, Creator> creators_;
};

}  // namespace cosmocad::document
