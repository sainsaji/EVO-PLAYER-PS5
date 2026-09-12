#ifndef EVO_MODAL_DIALOG_SCREEN_HPP
#define EVO_MODAL_DIALOG_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"
#include <string>

namespace evo {

enum class ModalType {
    ExitConfirm,
    ResumePrompt
};

/**
 * @brief Discrete states for modal prompts and confirmation dialogues.
 */
enum class ModalDialogState : int {
    Inactive = 0,
    Prompting = 1,
    Confirmed = 2,
    Cancelled = 3
};

/**
 * @brief Discrete triggers for modal state transitions.
 */
enum class ModalDialogEvent : int {
    Open = 0,
    Confirm = 1,
    Cancel = 2,
    Dismiss = 3
};

class ModalDialogScreen : public StatefulScreen {
public:
    explicit ModalDialogScreen(ModalType type);
    ~ModalDialogScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_modalFsm; }
    const IStateMachine* getStateMachine() const override { return &m_modalFsm; }

    ModalDialogState getModalState() const {
        return m_modalFsm.getCurrentState();
    }

    ScreenId getScreenId() const override;
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    void setResumeTimestamp(double seconds) { m_resumeTimestamp = seconds; }
    void setResumeTarget(const std::string& path, double posSeconds, double durationSeconds) {
        m_resumePath = path;
        m_resumeTimestamp = posSeconds;
        m_resumeDuration = durationSeconds;
    }

private:
    void initModalStateMachine();
    void executeAction(int actionIndex);

    ModalType m_type;
    StateMachine<ModalDialogState, ModalDialogEvent> m_modalFsm;
    int m_focusedButton = 1; // Default to primary action (STOP / RESUME)
    std::string m_resumePath;
    double m_resumeTimestamp = 0.0;
    double m_resumeDuration = 0.0;
};

} // namespace evo

#endif // EVO_MODAL_DIALOG_SCREEN_HPP
