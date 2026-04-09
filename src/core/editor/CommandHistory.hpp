#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Command.hpp"
#include "core/events/Signal.hpp"

namespace core {

class CommandHistory {
public:
    void execute(CommandPtr command) {
        if (!command) {
            return;
        }

        command->apply();
        redoStack_.clear();

        if (!undoStack_.empty() &&
            !command->mergeKey().empty() &&
            undoStack_.back()->mergeWith(*command)) {
            changed_.notify();
            return;
        }

        undoStack_.push_back(std::move(command));
        changed_.notify();
    }

    bool undo() {
        if (undoStack_.empty()) {
            return false;
        }

        auto command = std::move(undoStack_.back());
        undoStack_.pop_back();
        command->undo();
        redoStack_.push_back(std::move(command));
        changed_.notify();
        return true;
    }

    bool redo() {
        if (redoStack_.empty()) {
            return false;
        }

        auto command = std::move(redoStack_.back());
        redoStack_.pop_back();
        command->apply();
        undoStack_.push_back(std::move(command));
        changed_.notify();
        return true;
    }

    void clear() {
        undoStack_.clear();
        redoStack_.clear();
        changed_.notify();
    }

    [[nodiscard]] bool canUndo() const {
        return !undoStack_.empty();
    }

    [[nodiscard]] bool canRedo() const {
        return !redoStack_.empty();
    }

    [[nodiscard]] const ICommand* latestUndo() const {
        return undoStack_.empty() ? nullptr : undoStack_.back().get();
    }

    Signal<>& changed() {
        return changed_;
    }

private:
    std::vector<CommandPtr> undoStack_;
    std::vector<CommandPtr> redoStack_;
    Signal<> changed_{};
};

}  // namespace core
