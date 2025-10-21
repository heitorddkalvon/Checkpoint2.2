#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include "esp_system.h" // para ESP.restart()

// =====================================================
// IDENTIFICAÇÃO
#define ALUNO_NOME "Heitor"
#define ALUNO_RM   "88594"

// =====================================================
// CONFIGURAÇÕES
#define TAMANHO_FILA              5
#define TEMPO_LIMITE_RECEPCAO_MS  5000
#define WDT_TIMEOUT_MS            3000

// =====================================================
// FLAGS (EventGroup)
#define BIT_GERADOR_ATIVO   (1 << 0)
#define BIT_RECEPTOR_ATIVO  (1 << 1)
#define BIT_RECUPERANDO     (1 << 2)

// =====================================================
// VARIÁVEIS GLOBAIS
QueueHandle_t xFilaDados;
EventGroupHandle_t xEventGroup;

TaskHandle_t xTaskGerador, xTaskReceptor, xTaskSupervisor;

volatile TickType_t xFeedWDT_Gerador = 0;
volatile TickType_t xFeedWDT_Receptor = 0;
volatile bool falhaCritica = false;

// =====================================================
// MÓDULO 1: GERAÇÃO DE DADOS
void vTaskGerador(void *pvParameters) {
  int valor = 0;
  const TickType_t intervalo = pdMS_TO_TICKS(200);

  xEventGroupSetBits(xEventGroup, BIT_GERADOR_ATIVO);

  while (true) {
    valor++;
    xFeedWDT_Gerador = xTaskGetTickCount();
    xEventGroupSetBits(xEventGroup, BIT_GERADOR_ATIVO);

    if (xQueueSend(xFilaDados, &valor, pdMS_TO_TICKS(10)) == pdPASS) {
      Serial.printf("{%s-RM:%s} [FILA] Dado %d enviado com sucesso!\n", ALUNO_NOME, ALUNO_RM, valor);
    } else {
      Serial.printf("{%s-RM:%s} [FILA] Cheia! Dado %d descartado.\n", ALUNO_NOME, ALUNO_RM, valor);
    }

    vTaskDelay(intervalo);
  }
}

// =====================================================
// MÓDULO 2: RECEPÇÃO DE DADOS
void vTaskReceptor(void *pvParameters) {
  int valorRecebido;
  const TickType_t tempoMaximo = pdMS_TO_TICKS(TEMPO_LIMITE_RECEPCAO_MS);
  int nivelFalha = 0;
  const int MAX_FALHAS = 3;

  xEventGroupSetBits(xEventGroup, BIT_RECEPTOR_ATIVO);

  while (true) {
    xFeedWDT_Receptor = xTaskGetTickCount();
    xEventGroupSetBits(xEventGroup, BIT_RECEPTOR_ATIVO);

    if (xQueueReceive(xFilaDados, &valorRecebido, tempoMaximo) == pdPASS) {
      // malloc e free obrigatórios
      int *pValor = (int *)malloc(sizeof(int));
      if (pValor != NULL) {
        *pValor = valorRecebido;
        Serial.printf("{%s-RM:%s} [TX] Valor recebido: %d -> Transmitindo...\n", ALUNO_NOME, ALUNO_RM, *pValor);
        free(pValor);
      }

      nivelFalha = 0;
      falhaCritica = false;
      xEventGroupClearBits(xEventGroup, BIT_RECUPERANDO);
    } else {
      nivelFalha++;
      if (nivelFalha <= MAX_FALHAS) {
        Serial.printf("{%s-RM:%s} [RX-AVISO] Timeout sem dados (%d). Tentando recuperar...\n", ALUNO_NOME, ALUNO_RM, nivelFalha);
        xEventGroupSetBits(xEventGroup, BIT_RECUPERANDO);
      } else {
        Serial.printf("{%s-RM:%s} [RX-ERRO] FALHA CRÍTICA! Módulo encerrado.\n", ALUNO_NOME, ALUNO_RM);
        falhaCritica = true;
        vTaskSuspend(NULL); // encerra a tarefa
      }
    }
  }
}

// =====================================================
// MÓDULO 3: SUPERVISÃO
void vTaskSupervisor(void *pvParameters) {
  const TickType_t intervalo = pdMS_TO_TICKS(1000);
  const TickType_t limiteWDT = pdMS_TO_TICKS(WDT_TIMEOUT_MS);

  while (true) {
    EventBits_t bits = xEventGroupGetBits(xEventGroup);
    TickType_t agora = xTaskGetTickCount();

    Serial.println("\n========= STATUS DO SISTEMA =========");
    Serial.printf("{%s-RM:%s} [STATUS] Gerador: %s\n", ALUNO_NOME, ALUNO_RM,
                  (bits & BIT_GERADOR_ATIVO) ? "ATIVO" : "INATIVO");
    Serial.printf("{%s-RM:%s} [STATUS] Receptor: %s\n", ALUNO_NOME, ALUNO_RM,
                  (bits & BIT_RECEPTOR_ATIVO) ? "ATIVO" : "INATIVO");
    if (bits & BIT_RECUPERANDO)
      Serial.printf("{%s-RM:%s} [STATUS] Receptor em RECUPERAÇÃO.\n", ALUNO_NOME, ALUNO_RM);
    if (falhaCritica)
      Serial.printf("{%s-RM:%s} [STATUS] FALHA CRÍTICA detectada.\n", ALUNO_NOME, ALUNO_RM);

    // Simula Watchdog Timer cooperativo
    TickType_t dtGer = agora - xFeedWDT_Gerador;
    TickType_t dtRec = agora - xFeedWDT_Receptor;

    if (dtGer > limiteWDT) {
      Serial.printf("{%s-RM:%s} [WDT] Gerador travado (%u ms). Reiniciando...\n",
                    ALUNO_NOME, ALUNO_RM, pdTICKS_TO_MS(dtGer));
      ESP.restart();
    }

    if (dtRec > limiteWDT) {
      Serial.printf("{%s-RM:%s} [WDT] Receptor travado (%u ms). Reiniciando...\n",
                    ALUNO_NOME, ALUNO_RM, pdTICKS_TO_MS(dtRec));
      ESP.restart();
    }

    Serial.println("=====================================\n");

    // Limpa flags para forçar atualização
    xEventGroupClearBits(xEventGroup, BIT_GERADOR_ATIVO | BIT_RECEPTOR_ATIVO);
    vTaskDelay(intervalo);
  }
}

// =====================================================
// SETUP E LOOP
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n{%s-RM:%s} [INIT] Sistema de Dados Robusto iniciado.\n", ALUNO_NOME, ALUNO_RM);

  // Cria fila e EventGroup
  xFilaDados = xQueueCreate(TAMANHO_FILA, sizeof(int));
  xEventGroup = xEventGroupCreate();

  if (!xFilaDados || !xEventGroup) {
    Serial.printf("{%s-RM:%s} [ERRO] Falha ao criar estruturas FreeRTOS!\n", ALUNO_NOME, ALUNO_RM);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  TickType_t agora = xTaskGetTickCount();
  xFeedWDT_Gerador = agora;
  xFeedWDT_Receptor = agora;

  // Criação das tarefas
  xTaskCreatePinnedToCore(vTaskGerador, "Gerador", 4096, NULL, 1, &xTaskGerador, 1);
  xTaskCreatePinnedToCore(vTaskReceptor, "Receptor", 4096, NULL, 2, &xTaskReceptor, 1);
  xTaskCreatePinnedToCore(vTaskSupervisor, "Supervisor", 4096, NULL, 3, &xTaskSupervisor, 0);
}

void loop() {
  vTaskDelete(NULL);
}
