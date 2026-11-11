// Транзакция — единственный способ изменить документ.
// Все изменения внутри одной транзакции либо фиксируются вместе (commit),
// либо полностью откатываются (rollback или выход из области видимости).
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "cosmocad/document/Document.h"

namespace cosmocad::document {

class Transaction {
public:
    Transaction(Document& document, std::string name);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool isActive() const { return active_; }

    Result<Uuid> add(std::unique_ptr<Object> object);
    Result<void> remove(const Uuid& id);
    Result<void> modify(const Uuid& id, const std::function<void(Object&)>& mutator);

    template <class T>
    Result<void> modifyAs(const Uuid& id, const std::function<void(T&)>& mutator) {
        if (!document_.findAs<T>(id))
            return core::makeError(core::ErrorCode::NotFound, "объект нужного типа не найден");
        return modify(id, [&](Object& object) { mutator(static_cast<T&>(object)); });
    }

    void commit();
    void rollback();

private:
    Document& document_;
    ChangeSet set_;
    bool active_ = false;
};

}  // namespace cosmocad::document
