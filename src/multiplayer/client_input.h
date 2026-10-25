#pragma once

#include "protocol.h"

namespace edf::net {
// Controller and native keyboard/mouse samples arrive on separate guest paths. Keep them
// independent so the last poll does not erase the other device's held controls.
void PublishLocalInput(const InputCommand& input);
void PublishLocalKeyboardInput(const InputCommand& input);
void ClearLocalKeyboardInput();
InputCommand PublishedLocalInput();
}  // namespace edf::net
