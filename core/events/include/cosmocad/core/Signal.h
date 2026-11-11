// Простое оповещение о событиях: подписчики получают вызов при notify().
// Не зависит от Qt, чтобы модель данных не была привязана к графической библиотеке.
#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace cosmocad::core {

template <class... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;
    using Connection = std::size_t;

    Connection connect(Slot slot) {
        slots_.push_back({nextId_, std::move(slot)});
        return nextId_++;
    }

    void disconnect(Connection id) {
        std::erase_if(slots_, [id](const auto& entry) { return entry.first == id; });
    }

    void notify(Args... args) const {
        auto copy = slots_;  // подписчик может отписаться во время вызова
        for (const auto& entry : copy) entry.second(args...);
    }

private:
    std::vector<std::pair<Connection, Slot>> slots_;
    Connection nextId_ = 1;
};

}  // namespace cosmocad::core
