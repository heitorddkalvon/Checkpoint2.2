#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include "esp_system.h" // ESP.restart()

// --- Definições Globais e Primitivas do FreeRTOS ---

QueueHandle_t xQueueDados; 

// Event group bits (mais apropriado para flags persistentes)
EventGroupHandle_t xEventGroup;
#define BIT_GERACAO_ATIVA  (1 << 0)
#define BIT_RECEPCAO_ATIVA (1 << 1)
#define BIT_RECOVERY       (1 << 2)

// Handlers das Tasks
TaskHandle_t xHandleGerador = NULL;
TaskHandle_t xHandleReceptor = NULL;
TaskHandle_t xHandleSupervisor = NULL;

// Timestamps de "feed" do WDT cooperativo
volatile TickType_t xUltimoFeedWDT_Gerador = 0;
volatile TickType_t xUltimoFeedWDT_Receptor = 0;
volatile bool bFalhaCriticaRecepcao = false;

// Configurações
#define TAMANHO_FILA 5
#define TEMPO_LIMITE_RECEPCAO_MS 5000 
#define WDT_TIMEOUT_MS 3000 // limite de WDT simulado

// --- 1. Gerador (Producer) ---
void vTaskGeracaoDados(void *pvParameters) {
    int iValorSequencial = 0;
    const TickType_t xDelay = pdMS_TO_TICKS(100);

    // Marca como ativo
    xEventGroupSetBits(xEventGroup, BIT_GERACAO_ATIVA);

    while (1) {
        iValorSequencial++;

        // Feed WDT (atualiza timestamp)
        xUltimoFeedWDT_Gerador = xTaskGetTickCount();

        // Também atualiza a flag de liveness no eventgroup a cada iteração
        xEventGroupSetBits(xEventGroup, BIT_GERACAO_ATIVA);

        // Tenta enviar sem bloquear muito; aqui esperamos até 10 ms antes de descartar
        BaseType_t xStatus = xQueueSend(xQueueDados, &iValorSequencial, pdMS_TO_TICKS(10));
        if (xStatus == pdPASS) {
            Serial.printf("Gerador: enviado %d\n", iValorSequencial);
        } else {
            Serial.printf("Gerador: FILA CHEIA. Descartado %d\n", iValorSequencial);
        }

        vTaskDelay(xDelay);
    }
}

// --- 2. Receptor (Consumer) ---
void vTaskRecepcaoDados(void *pvParameters) {
    int iValorRecebido;
    const TickType_t xTempoEsperaTicks = pdMS_TO_TICKS(TEMPO_LIMITE_RECEPCAO_MS);

    int iNivelFalha = 0;
    const int MAX_FALHAS_RECUPERAVEIS = 3;

    // Marca como ativo
    xEventGroupSetBits(xEventGroup, BIT_RECEPCAO_ATIVA);

    while (1) {
        // Feed WDT
        xUltimoFeedWDT_Receptor = xTaskGetTickCount();

        // Atualiza liveness
        xEventGroupSetBits(xEventGroup, BIT_RECEPCAO_ATIVA);

        if (xQueueReceive(xQueueDados, &iValorRecebido, xTempoEsperaTicks) == pdPASS) {
            // Evita malloc: usar variável local (stack) suficiente para um int
            int tmp = iValorRecebido;
            Serial.printf("Receptor: Recebido %d -> transmitindo (simulado)\n", tmp);

            // Reset falhas
            iNivelFalha = 0;
            bFalhaCriticaRecepcao = false;

            // Limpa flag recovery se estava setada
            xEventGroupClearBits(xEventGroup, BIT_RECOVERY);
        } else {
            // Timeout de recepção
            iNivelFalha++;
            if (iNivelFalha <= MAX_FALHAS_RECUPERAVEIS) {
                Serial.printf("Receptor: AVISO (nivel %d) - timeout de recepcao. Tentando recuperar.\n", iNivelFalha);
                bFalhaCriticaRecepcao = false;
                xEventGroupSetBits(xEventGroup, BIT_RECOVERY);
                // Aqui poderia haver uma ação de recuperação (reconfigurar periférico)
            } else {
                Serial.println("Receptor: FALHA CRÍTICA! max tentativas esgotado.");
                bFalhaCriticaRecepcao = true;
                // Opcional: suspender ou reiniciar a própria tarefa antes do WDT agir
                // vTaskSuspend(NULL); // comente/descomente conforme intenção
            }
        }
    }
}

// --- 3. Supervisor ---
void vTaskSupervisao(void *pvParameters) {
    const TickType_t xDelayStatus = pdMS_TO_TICKS(1000);
    const TickType_t xLimiteWDT = pdMS_TO_TICKS(WDT_TIMEOUT_MS);

    while (1) {
        // Lê bits do EventGroup (não remove)
        EventBits_t uxBits = xEventGroupGetBits(xEventGroup);

        Serial.println("\n--- Supervisor: status ---");
        Serial.printf("Gerador: %s\n", (uxBits & BIT_GERACAO_ATIVA) ? "ATIVO" : "INATIVO");
        Serial.printf("Receptor: %s\n", (uxBits & BIT_RECEPCAO_ATIVA) ? "ATIVO" : "INATIVO");
        if (uxBits & BIT_RECOVERY) Serial.println("Receptor: EM RECUPERACAO");
        if (bFalhaCriticaRecepcao) Serial.println("Receptor: FALHA CRITICA DETECTADA");

        // Verificacao WDT cooperativa
        TickType_t now = xTaskGetTickCount();
        TickType_t dtGer = now - xUltimoFeedWDT_Gerador;
        TickType_t dtRec = now - xUltimoFeedWDT_Receptor;

        Serial.println("--- Monitor WDT ---");
        if (dtGer > xLimiteWDT) {
            Serial.printf("WDT: Gerador nao alimentado por %u ms -> REBOOT\n", pdTICKS_TO_MS(dtGer));
            ESP.restart();
        } else {
            Serial.printf("Gerador OK (ult feed %u ms)\n", pdTICKS_TO_MS(dtGer));
        }

        if (dtRec > xLimiteWDT) {
            Serial.printf("WDT: Receptor nao alimentado por %u ms -> REBOOT\n", pdTICKS_TO_MS(dtRec));
            ESP.restart();
        } else {
            Serial.printf("Receptor OK (ult feed %u ms)\n", pdTICKS_TO_MS(dtRec));
        }

        Serial.println("-------------------------\n");

        // Como usamos event group para liveness, podemos limpar bits de ATIVO aqui
        // para forçar as tasks a setarem novamente em sua proxima iteracao
        xEventGroupClearBits(xEventGroup, BIT_GERACAO_ATIVA | BIT_RECEPCAO_ATIVA);

        vTaskDelay(xDelayStatus);
    }
}

// --- Setup / Loop ---
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Inicio sistema FreeRTOS - versao corrigida");

    xQueueDados = xQueueCreate(TAMANHO_FILA, sizeof(int));
    if (!xQueueDados) {
        Serial.println("ERRO: nao foi possivel criar fila");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Criar EventGroup
    xEventGroup = xEventGroupCreate();
    if (!xEventGroup) {
        Serial.println("ERRO: nao foi possivel criar event group");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Inicializa timestamps para evitar falso positivo
    TickType_t now = xTaskGetTickCount();
    xUltimoFeedWDT_Gerador = now;
    xUltimoFeedWDT_Receptor = now;

    // Cria tasks
    xTaskCreatePinnedToCore(vTaskGeracaoDados, "GeradorDados", 4096, NULL, 1, &xHandleGerador, 1);
    xTaskCreatePinnedToCore(vTaskRecepcaoDados, "ReceptorDados", 4096, NULL, 2, &xHandleReceptor, 1);
    xTaskCreatePinnedToCore(vTaskSupervisao, "Supervisor", 3072, NULL, 3, &xHandleSupervisor, 0);
}

void loop() {
    // Deleta a task loop() do Arduino - mantemos tudo em FreeRTOS tasks
    vTaskDelete(NULL);
}