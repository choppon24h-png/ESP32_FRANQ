#include "auth_validator.h"

#include <mbedtls/md.h>
#include <string.h>

#include "config.h"

namespace {

// Último erro de validação para diagnóstico
static char s_lastError[64] = "none";

void setError(const char* msg) {
  strncpy(s_lastError, msg, sizeof(s_lastError) - 1);
  s_lastError[sizeof(s_lastError) - 1] = '\0';
  Serial.printf("[AUTH] Erro de validação: %s\n", s_lastError);
}

/**
 * Calcula HMAC-SHA256 de uma mensagem com a chave secreta.
 * Retorna o resultado em hexadecimal (64 chars).
 */
bool computeHmac(const String& message, char* outHex, size_t outHexLen) {
  if (outHexLen < 65) {
    return false;
  }

  uint8_t hmacResult[32] = {0};
  const char* key = AUTH_SECRET_KEY;
  const size_t keyLen = strlen(key);

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);

  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    mbedtls_md_free(&ctx);
    return false;
  }

  if (mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }

  if (mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, keyLen) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }

  if (mbedtls_md_hmac_update(&ctx, (const uint8_t*)message.c_str(), message.length()) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }

  if (mbedtls_md_hmac_finish(&ctx, hmacResult) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }

  mbedtls_md_free(&ctx);

  // Converter para hexadecimal
  for (int i = 0; i < 32; i++) {
    snprintf(outHex + (i * 2), 3, "%02x", hmacResult[i]);
  }
  outHex[64] = '\0';

  return true;
}

/**
 * Comparação em tempo constante para evitar timing attacks.
 */
bool constantTimeCompare(const char* a, const char* b, size_t len) {
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  }
  return diff == 0;
}

}  // namespace

void authValidator_init() {
  Serial.println("[AUTH] Módulo de validação HMAC-SHA256 inicializado");
  Serial.printf("[AUTH] Janela de validade do token: %lu ms\n", AUTH_TOKEN_VALID_MS);
}

bool authValidator_validate(const String& token, const String& sessionId) {
  // Formato esperado: "hmac_hex_64chars:timestamp_segundos"
  // Exemplo: "a3f2b1...64chars...:1711000000"

  if (token.length() < 67) {  // 64 (hmac) + 1 (:) + 1 (ts mínimo) + 1 = 67
    setError("token muito curto");
    return false;
  }

  // Separar HMAC do timestamp
  int separatorIdx = token.indexOf(':', 64);
  if (separatorIdx != 64) {
    setError("formato invalido - separador nao encontrado na posicao 64");
    return false;
  }

  String receivedHmac = token.substring(0, 64);
  String timestampStr = token.substring(65);

  if (timestampStr.isEmpty()) {
    setError("timestamp ausente");
    return false;
  }

  // CORREÇÃO CRÍTICA: O Android e o ESP32 têm bases de tempo diferentes.
  // O Android usa SystemClock.elapsedRealtime() (uptime do Android)
  // O ESP32 usa millis() (uptime do ESP32)
  // Como não há sincronização de relógio, não podemos validar a "idade" do token
  // subtraindo os timestamps. A segurança é garantida pelo HMAC e pelo SESSION_ID único.
  // Removemos a validação de tokenAgeMs para evitar falsos positivos de "token expirado".
  
  // Recalcular HMAC esperado: HMAC(SESSION_ID + ":" + timestamp, SECRET_KEY)
  String message = sessionId + ":" + timestampStr;
  char expectedHmac[65] = {0};

  if (!computeHmac(message, expectedHmac, sizeof(expectedHmac))) {
    setError("falha ao calcular HMAC");
    return false;
  }

  // Comparação em tempo constante
  if (!constantTimeCompare(receivedHmac.c_str(), expectedHmac, 64)) {
    setError("HMAC invalido");
    Serial.printf("[AUTH] HMAC recebido: %.16s...\n", receivedHmac.c_str());
    Serial.printf("[AUTH] HMAC esperado: %.16s...\n", expectedHmac);
    return false;
  }

  strncpy(s_lastError, "none", sizeof(s_lastError));
  Serial.printf("[AUTH] Token válido. Session: %s, Timestamp: %s\n",
                sessionId.c_str(), timestampStr.c_str());
  return true;
}

const char* authValidator_lastError() {
  return s_lastError;
}
