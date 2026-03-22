#pragma once

#include <Arduino.h>

/**
 * flow_sensor — Sensor de fluxo YF-S401 por interrupção.
 *
 * Mede o volume real dispensado contando pulsos do sensor.
 * Cada pulso corresponde a (1000 / FLOW_PULSOS_POR_LITRO) ml.
 */

void flowSensor_init();
void flowSensor_reset();
void flowSensor_enable();
void flowSensor_disable();
uint32_t flowSensor_getPulsos();
uint32_t flowSensor_getMl();
uint32_t flowSensor_getUltimoPulsoMs();
bool flowSensor_isTimeout();
void flowSensor_setPulsosLitro(uint32_t pl);
uint32_t flowSensor_calcularAlvo(uint32_t ml);
