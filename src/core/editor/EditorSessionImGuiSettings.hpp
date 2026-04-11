#pragma once

namespace core {

struct EditorSession;

void registerEditorSessionImGuiSettings(EditorSession& session);
void markEditorSessionImGuiSettingsDirty();

inline void setPersistedEditorSessionFlag(bool& target, bool value) {
    if (target == value) {
        return;
    }

    target = value;
    markEditorSessionImGuiSettingsDirty();
}

}  // namespace core
