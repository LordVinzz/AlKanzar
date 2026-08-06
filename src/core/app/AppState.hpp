#pragma once

#include "AppMode.hpp"

namespace core {

struct EngineServices;

class IAppState {
public:
    virtual ~IAppState() = default;

    [[nodiscard]] virtual AppMode mode() const = 0;
    virtual void onEnter(EngineServices& services) = 0;
    virtual void onExit(EngineServices& services) = 0;
    virtual void update(EngineServices& services) = 0;
    virtual void renderUi(EngineServices& services) = 0;
};

class BootstrapState final : public IAppState {
public:
    [[nodiscard]] AppMode mode() const override { return AppMode::Bootstrap; }
    void onEnter(EngineServices& services) override;
    void onExit(EngineServices& services) override;
    void update(EngineServices& services) override;
    void renderUi(EngineServices& services) override;
};

class GameplayState final : public IAppState {
public:
    [[nodiscard]] AppMode mode() const override { return AppMode::Gameplay; }
    void onEnter(EngineServices& services) override;
    void onExit(EngineServices& services) override;
    void update(EngineServices& services) override;
    void renderUi(EngineServices& services) override;
};

class EditorState final : public IAppState {
public:
    [[nodiscard]] AppMode mode() const override { return AppMode::Editor; }
    void onEnter(EngineServices& services) override;
    void onExit(EngineServices& services) override;
    void update(EngineServices& services) override;
    void renderUi(EngineServices& services) override;
};

class TestToolState final : public IAppState {
public:
    [[nodiscard]] AppMode mode() const override { return AppMode::TestTool; }
    void onEnter(EngineServices& services) override;
    void onExit(EngineServices& services) override;
    void update(EngineServices& services) override;
    void renderUi(EngineServices& services) override;
};

class ShutdownState final : public IAppState {
public:
    [[nodiscard]] AppMode mode() const override { return AppMode::Shutdown; }
    void onEnter(EngineServices& services) override;
    void onExit(EngineServices& services) override;
    void update(EngineServices& services) override;
    void renderUi(EngineServices& services) override;
};

class AppStateCollection {
public:
    [[nodiscard]] IAppState& forMode(AppMode mode);

private:
    BootstrapState bootstrap_{};
    GameplayState gameplay_{};
    EditorState editor_{};
    TestToolState testTool_{};
    ShutdownState shutdown_{};
};

}  // namespace core
