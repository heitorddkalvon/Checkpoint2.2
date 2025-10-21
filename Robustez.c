#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include "esp_system.h" // Para ESP.restart()

// ---------------- DEFINIÇÕES GLOBAIS ----------------
#define TAMANHO_FILA 5
#define TEMPO_LIMITE_RECEPCAO_MS 5000
#define WDT_TIMEOUT_MS 3000

// Bits de status
#define BIT_GERADOR_ATIVO   (1 << 0)
#define BIT_RECEPTOR_ATIVO  (1 << 1)
#define BIT_RECUPERANDO     (1 << 2)

QueueHandle_t xQueueDados;
EventGroupHandle_t xEventGroup;

TaskHandle_t xHandleGerador, xHandleReceptor, xHandleSupervisor;

volatile TickType_t xFeedWDT_Gerador = 0;
volatile TickType_t xFeedWDT_Receptor = 0;
volatile bool falhaCritica = false;

// ---------------- 1. MÓDULO DE GERAÇÃO ----------------
void vTaskGerador(void *pvParameters) {
  int valor = 0;
  const TickType_t delayEnvio = pdMS_TO_TICKS(100);

  xEventGroupSetBits(xEventGroup, BIT_GERADOR_ATIVO);

  while (true) {
    valor++;
    xFeedWDT_Gerador = xTaskGetTickCount();
    xEventGroupSetBits(xEventGroup, BIT_GERADOR_ATIVO);

    if (xQueueSend(xQueueDados, &valor, pdMS_TO_TICKS(10)) == pdPASS) {
      Serial.printf("Gerador: enviado %d\n", valor);
    } else {
      Serial.printf("Gerador: FILA CHEIA, valor %d descartado\n", valor);
    }

    vTaskDelay(delayEnvio);
  }
}

// ---------------- 2. MÓDULO DE RECEPÇÃO ----------------
void vTaskReceptor(void *pvParameters) {
  int valorRecebido;
  const TickType_t tempoLimite = pdMS_TO_TICKS(TEMPO_LIMITE_RECEPCAO_MS);
  int nivelFalha = 0;
  const int MAX_FALHAS = 3;

  xEventGroupSetBits(xEventGroup, BIT_RECEPTOR_ATIVO);

  while (true) {
    xFeedWDT_Receptor = xTaskGetTickCount();
    xEventGroupSetBits(xEventGroup, BIT_RECEPTOR_ATIVO);

    if (xQueueReceive(xQueueDados, &valorRecebido, tempoLimite) == pdPASS) {
      // Simula uso de memória dinâmica
      int *pValor = (int *)malloc(sizeof(int));
      if (pValor) {
        *pValor = valorRecebido;
        Serial.printf("Receptor: recebido %d -> transmitindo (simulado)\n", *pValor);
        free(pValor);
      }

      nivelFalha = 0;
      falhaCritica = false;
      xEventGroupClearBits(xEventGroup, BIT_RECUPERANDO);
    } else {
      nivelFalha++;
      if (nivelFalha <= MAX_FALHAS) {
        Serial.printf("Receptor: aviso %d - timeout sem dados. Recuperando...\n", nivelFalha);
        xEventGroupSetBits(xEventGroup, BIT_RECUPERANDO);
      } else {
        Serial.println("Receptor: FALHA CRÍTICA! Encerrando módulo de recepção.");
        falhaCritica = true;
        vTaskSuspend(NULL);
      }
    }
  }
}

// ---------------- 3. MÓDULO DE SUPERVISÃO ----------------
void vTaskSupervisor(void *pvParameters) {
  const TickType_t intervalo = pdMS_TO_TICKS(1000);
  const TickType_t limiteWDT = pdMS_TO_TICKS(WDT_TIMEOUT_MS);

  while (true) {
    EventBits_t bits = xEventGroupGetBits(xEventGroup);
    TickType_t agora = xTaskGetTickCount();

    Serial.println("\n--- STATUS DO SISTEMA ---");
    Serial.printf("Gerador: %s\n", (bits & BIT_GERADOR_ATIVO) ? "ATIVO" : "INATIVO");
    Serial.printf("Receptor: %s\n", (bits & BIT_RECEPTOR_ATIVO) ? "ATIVO" : "INATIVO");
    if (bits & BIT_RECUPERANDO) Serial.println("Receptor: EM RECUPERAÇÃO");
    if (falhaCritica) Serial.println("Receptor: FALHA CRÍTICA DETECTADA");

    TickType_t dtGer = agora - xFeedWDT_Gerador;
    TickType_t dtRec = agora - xFeedWDT_Receptor;

    Serial.println("--- Watchdog ---");
    if (dtGer > limiteWDT) {
      Serial.printf("WDT: Gerador inativo por %u ms -> Reiniciando sistema...\n", pdTICKS_TO_MS(dtGer));
      ESP.restart();
    } else {
      Serial.printf("Gerador OK (último feed %u ms)\n", pdTICKS_TO_MS(dtGer));
    }

    if (dtRec > limiteWDT) {
      Serial.printf("WDT: Receptor inativo por %u ms -> Reiniciando sistema...\n", pdTICKS_TO_MS(dtRec));
      ESP.restart();
    } else {
      Serial.printf("Receptor OK (último feed %u ms)\n", pdTICKS_TO_MS(dtRec));
    }

    Serial.println("-------------------------\n");

    // Limpa flags ativas (obrigando as tarefas a renová-las)
    xEventGroupClearBits(xEventGroup, BIT_GERADOR_ATIVO | BIT_RECEPTOR_ATIVO);

    vTaskDelay(intervalo);
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Sistema de Dados Robusto - FreeRTOS ===");

  xQueueDados = xQueueCreate(TAMANHO_FILA, sizeof(int));
  xEventGroup = xEventGroupCreate();

  if (!xQueueDados || !xEventGroup) {
    Serial.println("Erro ao criar primitivas FreeRTOS!");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  TickType_t agora = xTaskGetTickCount();
  xFeedWDT_Gerador = agora;
  xFeedWDT_Receptor = agora;

  xTaskCreatePinnedToCore(vTaskGerador, "Gerador", 4096, NULL, 1, &xHandleGerador, 1);
  xTaskCreatePinnedToCore(vTaskReceptor, "Receptor", 4096, NULL, 2, &xHandleReceptor, 1);
  xTaskCreatePinnedToCore(vTaskSupervisor, "Supervisor", 4096, NULL, 3, &xHandleSupervisor, 0);
}

void loop() {
  vTaskDelete(NULL);
}
