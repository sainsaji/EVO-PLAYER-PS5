#ifndef EVO_I_SURROUND_TEST_SERVICE_HPP
#define EVO_I_SURROUND_TEST_SERVICE_HPP

#include "evo/Common.hpp"

namespace evo {

/**
 * @brief Interface for multi-channel surround audio diagnostic tests.
 */
class ISurroundTestService {
public:
    virtual ~ISurroundTestService() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void triggerTone(bool is51Layout, int channelIndex) = 0;
    virtual bool isActive() const = 0;
    virtual int getCurrentChannel() const = 0;
    virtual bool is51Layout() const = 0;
    virtual void set51Layout(bool is51) = 0;
};

} // namespace evo

#endif // EVO_I_SURROUND_TEST_SERVICE_HPP
