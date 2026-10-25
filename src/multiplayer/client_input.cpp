#include "client_input.h"

#include <algorithm>
#include <mutex>

namespace edf::net {
namespace {
std::mutex g_input_mutex;
InputCommand g_input;
InputCommand g_pad_input;
InputCommand g_keyboard_input;

void RebuildInput() {
  g_input = {};
  g_input.buttons = g_pad_input.buttons | g_keyboard_input.buttons;
  g_input.left_trigger = std::max(g_pad_input.left_trigger, g_keyboard_input.left_trigger);
  g_input.right_trigger = std::max(g_pad_input.right_trigger, g_keyboard_input.right_trigger);
  // Keyboard movement and vehicle mouse aim override the physical stick when nonzero,
  // matching the guest input hook's channel merge behavior.
  g_input.move_x = g_keyboard_input.move_x != 0 ? g_keyboard_input.move_x : g_pad_input.move_x;
  g_input.move_y = g_keyboard_input.move_y != 0 ? g_keyboard_input.move_y : g_pad_input.move_y;
  g_input.aim_x = g_keyboard_input.aim_x != 0 ? g_keyboard_input.aim_x : g_pad_input.aim_x;
  g_input.aim_y = g_keyboard_input.aim_y != 0 ? g_keyboard_input.aim_y : g_pad_input.aim_y;
}
}

void PublishLocalInput(const InputCommand& input) {
  std::lock_guard lock(g_input_mutex);
  g_pad_input = input;
  RebuildInput();
}

void PublishLocalKeyboardInput(const InputCommand& input) {
  std::lock_guard lock(g_input_mutex);
  g_keyboard_input = input;
  RebuildInput();
}

void ClearLocalKeyboardInput() {
  PublishLocalKeyboardInput({});
}

InputCommand PublishedLocalInput() {
  std::lock_guard lock(g_input_mutex);
  return g_input;
}
}  // namespace edf::net
