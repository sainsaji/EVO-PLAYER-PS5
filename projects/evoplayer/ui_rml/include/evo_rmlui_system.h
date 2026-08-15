#pragma once
#include <RmlUi/Core/SystemInterface.h>

class EvoSystemInterface : public Rml::SystemInterface {
public:
    EvoSystemInterface();
    virtual ~EvoSystemInterface();

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
};
