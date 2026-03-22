#include "command_queue.h"

#include "protocol.h"

namespace {

QueueHandle_t g_commandQueue = nullptr;

}  // namespace

void cmdQueue_init() {
  if (!g_commandQueue) {
    g_commandQueue = xQueueCreate(COMMAND_QUEUE_SIZE, sizeof(QueuedCommand));
  }
  configASSERT(g_commandQueue != nullptr);
}

bool cmdQueue_enqueue(const String& rawCommand) {
  if (!g_commandQueue || rawCommand.isEmpty()) {
    return false;
  }

  QueuedCommand item = {};
  rawCommand.substring(0, RX_BUFFER_SIZE - 1).toCharArray(item.raw, sizeof(item.raw));
  return xQueueSend(g_commandQueue, &item, 0) == pdTRUE;
}

bool cmdQueue_dequeue(String& rawCommand, TickType_t timeoutTicks) {
  if (!g_commandQueue) {
    return false;
  }

  QueuedCommand item = {};
  if (xQueueReceive(g_commandQueue, &item, timeoutTicks) != pdTRUE) {
    return false;
  }

  rawCommand = String(item.raw);
  return true;
}

void cmdQueue_clear() {
  if (!g_commandQueue) {
    return;
  }

  QueuedCommand item = {};
  while (xQueueReceive(g_commandQueue, &item, 0) == pdTRUE) {
  }
}

UBaseType_t cmdQueue_size() {
  return g_commandQueue ? uxQueueMessagesWaiting(g_commandQueue) : 0;
}
