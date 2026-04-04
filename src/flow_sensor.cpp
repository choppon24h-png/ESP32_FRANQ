#include "flow_sensor.h"

#include <Arduino.h>

#include "config.h"

namespace {

volatile uint32_t g_pulseCount = 0;
volatile uint32_t g_lastPulseMs = 0;
uint32_t g_enabledAtMs = 0;  // v2.3.1 FIX: timestamp de quando o sensor foi habilitado
bool g_enabled = false;
uint32_t g_pulsosLitro = FLOW_PULSOS_POR_LITRO;

void IRAM_ATTR flowISR() {
  g_pulseCount++;
  g_lastPulseMs = (uint32_t)millis();
}

}  // namespace

void flowSensor_init() {
  pinMode(PINO_SENSOR_FLUSO, INPUT_PULLUP);
  g_pulseCount = 0;
  g_lastPulseMs = 0;
  g_enabledAtMs = 0;
  g_enabled = false;
  g_pulsosLitro = FLOW_PULSOS_POR_LITRO;
  Serial.printf("[FLOW] Sensor inicializado. Pino: %d, Pulsos/L: %u\n",
                PINO_SENSOR_FLUSO, g_pulsosLitro);
}

void flowSensor_reset() {
  noInterrupts();
  g_pulseCount = 0;
  g_lastPulseMs = (uint32_t)millis();
  interrupts();
}

void flowSensor_enable() {
  if (g_enabled) return;
  flowSensor_reset();
  g_enabledAtMs = (uint32_t)millis();  // v2.3.1 FIX: marca o momento exato de habilitação
  attachInterrupt(digitalPinToInterrupt(PINO_SENSOR_FLUSO), flowISR, RISING);
  g_enabled = true;
  Serial.printf("[FLOW] Sensor habilitado em t=%lu ms\n", (unsigned long)g_enabledAtMs);
}

void flowSensor_disable() {
  if (!g_enabled) return;
  detachInterrupt(digitalPinToInterrupt(PINO_SENSOR_FLUSO));
  g_enabled = false;
  g_enabledAtMs = 0;
  Serial.println("[FLOW] Sensor desabilitado");
}

uint32_t flowSensor_getPulsos() {
  noInterrupts();
  const uint32_t count = g_pulseCount;
  interrupts();
  return count;
}

uint32_t flowSensor_getMl() {
  const uint32_t pulsos = flowSensor_getPulsos();
  if (g_pulsosLitro == 0) return 0;
  return (uint32_t)((uint64_t)pulsos * 1000ULL / g_pulsosLitro);
}

uint32_t flowSensor_getUltimoPulsoMs() {
  noInterrupts();
  const uint32_t t = g_lastPulseMs;
  interrupts();
  return t;
}

bool flowSensor_isTimeout() {
  const uint32_t last = flowSensor_getUltimoPulsoMs();
  const uint32_t now = (uint32_t)millis();
  // v2.3.1 FIX: usa tempo relativo ao momento de habilitação do sensor,
  // não millis() global. O bug original usava "if (now < FLOW_NO_PULSE_TIMEOUT_MS)"
  // que é sempre falso quando millis() > 10s (após o boot), fazendo o timeout
  // disparar imediatamente se o último pulso foi há mais de 10s (ex: g_lastPulseMs=0
  // após flowSensor_reset() quando millis() já é grande).
  // A correção: só considera timeout se já passou FLOW_NO_PULSE_TIMEOUT_MS
  // desde que o sensor foi habilitado para esta dispensação.
  if (g_enabledAtMs == 0) return false;
  const uint32_t sinceEnable = now - g_enabledAtMs;
  if (sinceEnable < FLOW_NO_PULSE_TIMEOUT_MS) return false;
  return (now - last) >= FLOW_NO_PULSE_TIMEOUT_MS;
}

void flowSensor_setPulsosLitro(uint32_t pl) {
  if (pl == 0) return;
  g_pulsosLitro = pl;
  Serial.printf("[FLOW] Calibração atualizada: %u pulsos/litro\n", pl);
}

uint32_t flowSensor_calcularAlvo(uint32_t ml) {
  return (uint32_t)((uint64_t)ml * g_pulsosLitro / 1000ULL);
}
