#pragma once

#include <Arduino.h>

/**
 * auth_validator — Validação de tokens HMAC-SHA256 para autenticação segura.
 *
 * O app Android deve gerar o token assim:
 *   token = HMAC-SHA256(SESSION_ID + ":" + timestamp_segundos, AUTH_SECRET_KEY)
 *   payload = token_hex + ":" + timestamp_segundos
 *
 * O firmware valida:
 *   1. Recalcula o HMAC com a mesma chave secreta
 *   2. Compara com o token recebido (comparação em tempo constante)
 *   3. Verifica se o timestamp não expirou (janela de AUTH_TOKEN_VALID_MS)
 *
 * Isso garante que apenas o app Android com a chave correta pode autenticar.
 */

/**
 * Inicializa o módulo de validação de autenticação.
 */
void authValidator_init();

/**
 * Valida um token de autenticação recebido via BLE.
 *
 * @param token    Token recebido no formato "hmac_hex:timestamp_segundos"
 * @param sessionId SESSION_ID da sessão atual
 * @return true se o token é válido e não expirou, false caso contrário
 */
bool authValidator_validate(const String& token, const String& sessionId);

/**
 * Retorna a descrição do último erro de validação para log.
 */
const char* authValidator_lastError();
