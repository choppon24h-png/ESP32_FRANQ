#!/usr/bin/env python3
"""
CHOPPON — Teste Manual de Dispensação BLE
==========================================
Testa o fluxo completo de dispensação sem o Android.
Registra todo o fluxo em log para diagnóstico.

INSTALAR:
    pip install bleak

EXECUTAR:
    python teste_chopp_ble.py

CONFIGURAR as variáveis abaixo conforme seu ambiente.
"""

import asyncio
import hmac
import hashlib
import time
import logging
import sys
from datetime import datetime
from bleak import BleakClient, BleakScanner

# ─── CONFIGURAÇÃO — AJUSTE AQUI ───────────────────────────────────────────────
MAC_ESP32        = "DC:B4:D9:99:3B:96"   # MAC do ESP32 (ver logs Android)
HMAC_SECRET_KEY  = "Choppon103614@"       # config.h → AUTH_SECRET_KEY
VOLUME_ML        = 300                    # ml a dispensar (teste com 50 primeiro!)
SESSION_ID       = f"SES_TEST_{int(time.time()) % 10000:04d}"
IMEI_FAKE        = "TEST_PC_001"
TIMEOUT_AUTH     = 5.0    # segundos para AUTH_OK
TIMEOUT_READY    = 5.0    # segundos para READY_OK
TIMEOUT_ACK      = 5.0    # segundos para ACK
TIMEOUT_DISPENSA = 35.0   # segundos para DONE (máx 30s no firmware)
PING_INTERVAL    = 2.0    # segundos entre PINGs de keepalive
GUARD_BAND_MS    = 0.95   # segundos após READY_OK antes do SERVE
# ──────────────────────────────────────────────────────────────────────────────

SERVICE_UUID = "7f0a0001-7b6b-4b5f-9d3e-3c7b9f100001"
RX_UUID      = "7f0a0002-7b6b-4b5f-9d3e-3c7b9f100001"
TX_UUID      = "7f0a0003-7b6b-4b5f-9d3e-3c7b9f100001"

# ─── LOGGING ──────────────────────────────────────────────────────────────────
timestamp_inicio = datetime.now().strftime("%Y%m%d_%H%M%S")
log_file = f"choppon_test_{timestamp_inicio}.log"

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s.%(msecs)03d  %(message)s",
    datefmt="%H:%M:%S",
    handlers=[
        logging.FileHandler(log_file, encoding="utf-8"),
        logging.StreamHandler(sys.stdout)
    ]
)
log = logging.getLogger("CHOPPON")

# ─── ESTADO GLOBAL ────────────────────────────────────────────────────────────
respostas        = []
auth_ok_evt      = asyncio.Event()
ready_ok_evt     = asyncio.Event()
ack_evt          = asyncio.Event()
done_evt         = asyncio.Event()
flow_timeout_evt = asyncio.Event()
ping_count       = 0
ml_real_final    = 0
pulsos_log       = []   # [(timestamp, ml, pulsos)] vindos de VP: notificações

def ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]

def gerar_auth_token(imei: str) -> str:
    """Gera token HMAC-SHA256 idêntico ao Android."""
    timestamp = int(time.time())
    payload   = f"{imei}:{timestamp}"
    token     = hmac.new(
        HMAC_SECRET_KEY.encode("utf-8"),
        payload.encode("utf-8"),
        hashlib.sha256
    ).hexdigest()
    return f"{token}:{timestamp}"

def on_notify(sender, data: bytearray):
    """Callback de notificação BLE — thread BLE."""
    global ml_real_final
    msg = data.decode("utf-8", errors="replace").strip()
    log.info(f"← [RX ESP32] {msg}")
    respostas.append((ts(), msg))

    if msg.startswith("AUTH_OK"):
        auth_ok_evt.set()

    elif msg.startswith("READY_OK"):
        ready_ok_evt.set()

    elif msg.startswith("ACK|"):
        ack_evt.set()

    elif msg.startswith("DONE|"):
        partes = msg.split("|")
        if len(partes) >= 3:
            try:
                ml_real_final = int(partes[2])
            except ValueError:
                ml_real_final = -1
        done_evt.set()

    elif msg.startswith("WARN:FLOW_TIMEOUT"):
        log.warning("⚠️  FLOW_TIMEOUT — barril vazio ou sensor sem pulsos!")
        flow_timeout_evt.set()

    elif msg.startswith("VP:"):
        # Notificação de progresso: VP:<ml_atual>/<ml_alvo>
        pulsos_log.append((ts(), msg))
        log.info(f"   📊 Progresso: {msg}")

    elif msg.startswith("ERROR"):
        log.error(f"❌ ERRO ESP32: {msg}")
        if "NOT_READY" in msg:
            log.error("   → SERVE enviado sem READY prévio ou guard-band insuficiente")
        elif "INVALID_FORMAT" in msg:
            log.error("   → Comando malformado: sessionId vazio ou separador errado")
        elif "NOT_AUTHENTICATED" in msg:
            log.error("   → AUTH não foi enviado ou falhou")
        elif "BUSY" in msg:
            log.error("   → Já há uma dispensação em andamento")
        elif "HMAC_INVALID" in msg:
            log.error("   → HMAC_SECRET_KEY no script não bate com config.h do firmware")
        elif "VOLUME_EXCEEDED" in msg:
            log.error("   → Volume > 2000ml ou volume = 0")

async def enviar(client: BleakClient, cmd: str, descricao: str = "") -> bool:
    """Envia comando BLE e retorna True se ok."""
    log.info(f"→ [TX ANDROID] {cmd}   {descricao}")
    try:
        await client.write_gatt_char(RX_UUID, cmd.encode("utf-8"), response=True)
        return True
    except Exception as e:
        log.error(f"   Falha ao escrever: {e}")
        return False

async def keepalive_loop(client: BleakClient):
    """Envia PING a cada PING_INTERVAL segundos até DONE."""
    global ping_count
    while not done_evt.is_set() and not flow_timeout_evt.is_set():
        await asyncio.sleep(PING_INTERVAL)
        if done_evt.is_set() or flow_timeout_evt.is_set():
            break
        ping_count += 1
        cmd = f"PING|HB_{ping_count:04d}|{SESSION_ID}"
        ok = await enviar(client, cmd, f"keepalive #{ping_count}")
        if not ok:
            log.warning("   Falha no PING — link pode estar instável")

async def scan_device():
    """Escaneia e retorna o dispositivo com o MAC configurado."""
    log.info(f"Escaneando por {MAC_ESP32}...")
    device = await BleakScanner.find_device_by_address(MAC_ESP32, timeout=10.0)
    if device:
        log.info(f"✅ Dispositivo encontrado: {device.name} [{device.address}]")
    else:
        log.error(f"❌ Dispositivo {MAC_ESP32} não encontrado")
    return device

async def main():
    log.info("=" * 65)
    log.info("  CHOPPON — TESTE MANUAL DE DISPENSAÇÃO BLE")
    log.info("=" * 65)
    log.info(f"  MAC ESP32:       {MAC_ESP32}")
    log.info(f"  Volume:          {VOLUME_ML}ml")
    log.info(f"  Session ID:      {SESSION_ID}")
    log.info(f"  HMAC Key:        {HMAC_SECRET_KEY}")
    log.info(f"  Log salvo em:    {log_file}")
    log.info("=" * 65)

    # ── Scan ──────────────────────────────────────────────────────────────────
    device = await scan_device()
    if not device:
        log.error("Encerrando — dispositivo não encontrado.")
        log.info("Dica: verifique se o ESP32 está ligado e o MAC está correto.")
        return

    t_inicio_total = time.time()

    async with BleakClient(device, timeout=15.0) as client:
        log.info(f"\n✅ Conectado! MTU negociado: {client.mtu_size} bytes")

        # Verificar serviço
        servicos = [str(s.uuid) for s in client.services]
        if SERVICE_UUID not in servicos:
            log.error(f"❌ Serviço {SERVICE_UUID} não encontrado!")
            log.info(f"   Serviços disponíveis: {servicos}")
            return
        log.info(f"✅ Serviço BLE encontrado")

        # Habilitar notificações
        await client.start_notify(TX_UUID, on_notify)
        log.info("✅ Notificações TX habilitadas")
        await asyncio.sleep(0.3)

        # ── STEP 1: AUTH ──────────────────────────────────────────────────────
        log.info("\n" + "─" * 40)
        log.info("[PASSO 1/6] AUTENTICAÇÃO")
        log.info("─" * 40)

        token    = gerar_auth_token(IMEI_FAKE)
        cmd_auth = f"AUTH|{token}|CMD_A001|{IMEI_FAKE}"
        t0 = time.time()
        await enviar(client, cmd_auth, "HMAC-SHA256 authentication")

        try:
            await asyncio.wait_for(auth_ok_evt.wait(), timeout=TIMEOUT_AUTH)
            log.info(f"✅ AUTH_OK recebido em {(time.time()-t0)*1000:.0f}ms")
        except asyncio.TimeoutError:
            log.error(f"❌ TIMEOUT {TIMEOUT_AUTH}s — AUTH_OK não recebido")
            log.error("   Verificar: HMAC_SECRET_KEY no script vs config.h")
            log.error("   Verificar: timestamp do PC sincronizado (AUTH_TOKEN_VALID_MS=300s)")
            await client.stop_notify(TX_UUID)
            return

        await asyncio.sleep(0.2)

        # ── STEP 2: READY ─────────────────────────────────────────────────────
        log.info("\n" + "─" * 40)
        log.info("[PASSO 2/6] READY")
        log.info("─" * 40)

        cmd_ready = f"READY|CMD_R001|{SESSION_ID}"
        t0 = time.time()
        await enviar(client, cmd_ready, "preparar sessão")

        try:
            await asyncio.wait_for(ready_ok_evt.wait(), timeout=TIMEOUT_READY)
            log.info(f"✅ READY_OK recebido em {(time.time()-t0)*1000:.0f}ms")
        except asyncio.TimeoutError:
            log.error(f"❌ TIMEOUT {TIMEOUT_READY}s — READY_OK não recebido")
            await client.stop_notify(TX_UUID)
            return

        # ── STEP 3: Guard-band ────────────────────────────────────────────────
        log.info("\n" + "─" * 40)
        log.info(f"[PASSO 3/6] GUARD-BAND ({GUARD_BAND_MS*1000:.0f}ms)")
        log.info("─" * 40)
        await asyncio.sleep(GUARD_BAND_MS)
        log.info("✅ Guard-band concluído")

        # ── STEP 4: SERVE ─────────────────────────────────────────────────────
        log.info("\n" + "─" * 40)
        log.info(f"[PASSO 4/6] SERVE {VOLUME_ML}ml")
        log.info("─" * 40)

        cmd_serve = f"SERVE|{VOLUME_ML}|CMD_S001|{SESSION_ID}"
        t_serve = time.time()
        await enviar(client, cmd_serve, f"dispensar {VOLUME_ML}ml")

        try:
            await asyncio.wait_for(ack_evt.wait(), timeout=TIMEOUT_ACK)
            log.info(f"✅ ACK recebido em {(time.time()-t_serve)*1000:.0f}ms")
            log.info("   🍺 VÁLVULA ABERTA — chopp saindo!")
        except asyncio.TimeoutError:
            log.error(f"❌ TIMEOUT {TIMEOUT_ACK}s — ACK não recebido")
            log.error("   Possível causa: SERVE rejeitado (verificar últimas respostas)")
            log.info(f"   Últimas respostas: {[r[1] for r in respostas[-5:]]}")
            await client.stop_notify(TX_UUID)
            return

        # ── STEP 5: Aguardar DONE com keepalive ───────────────────────────────
        log.info("\n" + "─" * 40)
        log.info(f"[PASSO 5/6] DISPENSANDO — aguardando DONE (máx {TIMEOUT_DISPENSA}s)")
        log.info(f"   Enviando PING a cada {PING_INTERVAL}s para manter link BLE")
        log.info("─" * 40)

        keepalive_task = asyncio.create_task(keepalive_loop(client))

        try:
            await asyncio.wait_for(
                asyncio.gather(done_evt.wait(), return_exceptions=True),
                timeout=TIMEOUT_DISPENSA
            )
            keepalive_task.cancel()

            if done_evt.is_set():
                duracao = time.time() - t_serve
                log.info(f"\n✅ DONE recebido!")
                log.info(f"   Volume real dispensado: {ml_real_final}ml")
                log.info(f"   Volume solicitado:      {VOLUME_ML}ml")
                log.info(f"   Duração da dispensação: {duracao:.1f}s")
                log.info(f"   PINGs enviados:         {ping_count}")
                if ml_real_final > 0 and VOLUME_ML > 0:
                    erro_pct = abs(ml_real_final - VOLUME_ML) / VOLUME_ML * 100
                    log.info(f"   Erro de volume:         {erro_pct:.1f}%")
                    if erro_pct > 10:
                        log.warning(f"   ⚠️  Erro > 10% — calibrar FLOW_PULSOS_POR_LITRO")
                        log.warning(f"      Atual: 450 pulsos/L. Ajustar em config.h")

            elif flow_timeout_evt.is_set():
                log.warning("\n⚠️  FLOW_TIMEOUT — barril vazio ou sensor sem pulsos")
                log.warning("   Verificar: sensor YF-S401 conectado no pino 0?")
                log.warning("   Verificar: barril com chopp e pressão adequada?")

        except asyncio.TimeoutError:
            keepalive_task.cancel()
            log.error(f"\n❌ TIMEOUT DE {TIMEOUT_DISPENSA}s — DONE não recebido")
            log.error("   Válvula pode ainda estar aberta no ESP32!")
            log.error("   Enviando STOP de segurança...")

            cmd_stop = f"STOP|CMD_STOP001|{SESSION_ID}"
            await enviar(client, cmd_stop, "SEGURANÇA — forçar fechamento")
            await asyncio.sleep(1.0)

        # ── STEP 6: STOP se necessário ────────────────────────────────────────
        if not done_evt.is_set():
            log.info("\n[PASSO 6/6] STOP preventivo")
            cmd_stop = f"STOP|CMD_STOP001|{SESSION_ID}"
            await enviar(client, cmd_stop, "fechar válvula")
            await asyncio.sleep(0.5)
        else:
            log.info("\n[PASSO 6/6] Válvula já fechada pelo ESP32 (DONE recebido)")

        # ── RELATÓRIO ─────────────────────────────────────────────────────────
        duracao_total = time.time() - t_inicio_total
        log.info("\n" + "=" * 65)
        log.info("  RELATÓRIO COMPLETO DO TESTE")
        log.info("=" * 65)
        log.info(f"  Duração total:         {duracao_total:.1f}s")
        log.info(f"  Volume solicitado:     {VOLUME_ML}ml")
        log.info(f"  Volume dispensado:     {ml_real_final}ml")
        log.info(f"  PINGs de keepalive:   {ping_count}")
        log.info(f"  Progresso recebidos:  {len(pulsos_log)}")
        log.info(f"\n  Todas as mensagens BLE ({len(respostas)}):")
        for ts_r, msg in respostas:
            log.info(f"    [{ts_r}] {msg}")
        log.info(f"\n  Log completo salvo em: {log_file}")
        log.info("=" * 65)

        await client.stop_notify(TX_UUID)

    log.info("\n✅ Desconectado do ESP32. Teste finalizado.")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("\n⚠️  Teste interrompido pelo usuário.")
        log.info("   ATENÇÃO: a válvula pode ainda estar aberta no ESP32!")
        log.info("   Reconectar e enviar STOP manualmente se necessário.")
    except Exception as e:
        log.error(f"\n❌ Erro inesperado: {e}")
        import traceback
        traceback.print_exc()