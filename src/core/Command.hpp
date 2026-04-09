#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace core {

class ICommand {
public:
    virtual ~ICommand() = default;

    virtual void apply() = 0;
    virtual void undo() = 0;
    [[nodiscard]] virtual std::string label() const = 0;
    [[nodiscard]] virtual std::string mergeKey() const { return {}; }
    virtual bool mergeWith(const ICommand& newer) = 0;
};

template <typename TSnapshot>
class SnapshotCommand final : public ICommand {
public:
    using ApplyFn = std::function<void(const TSnapshot&)>;

    SnapshotCommand(
        std::string label,
        std::string mergeKey,
        TSnapshot before,
        TSnapshot after,
        ApplyFn applyFn,
        bool mergeable = true
    )
        : label_(std::move(label)),
          mergeKey_(std::move(mergeKey)),
          before_(std::move(before)),
          after_(std::move(after)),
          apply_(std::move(applyFn)),
          mergeable_(mergeable) {}

    void apply() override {
        apply_(after_);
    }

    void undo() override {
        apply_(before_);
    }

    [[nodiscard]] std::string label() const override {
        return label_;
    }

    [[nodiscard]] std::string mergeKey() const override {
        return mergeKey_;
    }

    bool mergeWith(const ICommand& newer) override {
        if (!mergeable_) {
            return false;
        }

        const auto* typed = dynamic_cast<const SnapshotCommand<TSnapshot>*>(&newer);
        if (typed == nullptr || typed->mergeKey_ != mergeKey_) {
            return false;
        }

        after_ = typed->after_;
        return true;
    }

private:
    std::string label_;
    std::string mergeKey_;
    TSnapshot before_;
    TSnapshot after_;
    ApplyFn apply_;
    bool mergeable_{true};
};

using CommandPtr = std::unique_ptr<ICommand>;

}  // namespace core
