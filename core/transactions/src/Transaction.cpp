#include "cosmocad/document/Transaction.h"

namespace cosmocad::document {

using core::ErrorCode;
using core::makeError;

Transaction::Transaction(Document& document, std::string name) : document_(document) {
    set_.name = std::move(name);
    if (!document_.transactionActive_) {
        document_.transactionActive_ = true;
        active_ = true;
    }
    // Если в документе уже идёт транзакция, эта остаётся неактивной,
    // и все её операции вернут ошибку InvalidState.
}

Transaction::~Transaction() {
    if (active_) rollback();
}

Result<Uuid> Transaction::add(std::unique_ptr<Object> object) {
    if (!active_) return makeError(ErrorCode::InvalidState, "транзакция не активна");
    if (!object) return makeError(ErrorCode::InvalidArgument, "пустой объект");
    if (document_.find(object->id())) return makeError(ErrorCode::AlreadyExists, "объект с таким номером уже есть");

    Change change;
    change.id = object->id();
    change.index = document_.objects_.size();
    change.after = object->clone();
    document_.insertAt(change.index, std::move(object));
    set_.changes.push_back(std::move(change));
    return set_.changes.back().id;
}

Result<void> Transaction::remove(const Uuid& id) {
    if (!active_) return makeError(ErrorCode::InvalidState, "транзакция не активна");
    auto index = document_.indexOf(id);
    if (index == document_.objects_.size()) return makeError(ErrorCode::NotFound, "объект не найден");

    Change change;
    change.id = id;
    change.index = index;
    change.before = document_.removeById(id);
    set_.changes.push_back(std::move(change));
    return {};
}

Result<void> Transaction::modify(const Uuid& id, const std::function<void(Object&)>& mutator) {
    if (!active_) return makeError(ErrorCode::InvalidState, "транзакция не активна");
    Object* object = document_.findMutable(id);
    if (!object) return makeError(ErrorCode::NotFound, "объект не найден");

    Change change;
    change.id = id;
    change.index = document_.indexOf(id);
    change.before = object->clone();
    mutator(*object);
    change.after = object->clone();
    set_.changes.push_back(std::move(change));
    return {};
}

void Transaction::commit() {
    if (!active_) return;
    active_ = false;
    document_.transactionActive_ = false;
    if (!set_.changes.empty()) document_.pushCommitted(std::move(set_));
}

void Transaction::rollback() {
    if (!active_) return;
    for (auto it = set_.changes.rbegin(); it != set_.changes.rend(); ++it) document_.applyBackward(*it);
    set_.changes.clear();
    active_ = false;
    document_.transactionActive_ = false;
}

}  // namespace cosmocad::document
