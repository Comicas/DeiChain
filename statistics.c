#include "statistics.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <string.h>

// Flag para término
static volatile sig_atomic_t should_terminate = 0;
static volatile sig_atomic_t sigusr1_received_statistics = 0;

// Handler para sinais
void statistics_signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        should_terminate = 1;
    } else if (sig == SIGUSR1) {
        log_message("STATISTICS: SIGNAL SIGUSR1 RECEIVED");
        sigusr1_received_statistics = 1;
    }
}

// Função para exibir estatísticas
void print_statistics(int *valid_blocks, int *invalid_blocks, float *avg_validation_time, int *credits, int num_miners) {
    #define MAX_STATS_LINES 100 // Máximo de linhas de estatísticas possíveis
    char **messages = (char**)malloc(MAX_STATS_LINES * sizeof(char*));
    int msg_idx = 0;
    
    // Aloca memória para cada linha da mensagem
    for (int i = 0; i < MAX_STATS_LINES; i++) {
        messages[i] = (char*)malloc(1024 * sizeof(char));
        if (!messages[i]) {
            perror("malloc messages");
            // Libera memória já alocada
            for (int j = 0; j < i; j++) {
                free(messages[j]);
            }
            free(messages);
            return;
        }
    }
    
    // Linha em branco
    strcpy(messages[msg_idx++], "");
    
    // Cabeçalho
    strcpy(messages[msg_idx++], "===== BLOCKCHAIN STATISTICS =====");
    
    // Estatísticas por minerador
    strcpy(messages[msg_idx++], "Statistics per Miner:");
    
    // Calcular totais enquanto processamos cada minerador
    int total_valid = 0;
    int total_invalid = 0;
    int total_credits = 0;
    float total_time = 0.0;
    int total_blocks = 0;
    
    for (int i = 0; i < num_miners; i++) {
        snprintf(messages[msg_idx++], 1024, "Miner %d:", i + 1);
        snprintf(messages[msg_idx++], 1024, "  Valid blocks: %d", valid_blocks[i]);
        snprintf(messages[msg_idx++], 1024, "  Invalid blocks: %d", invalid_blocks[i]);
        snprintf(messages[msg_idx++], 1024, "  Credits: %d", credits[i]);
        snprintf(messages[msg_idx++], 1024, "  Avg. validation time: %.6f seconds", avg_validation_time[i]);
        
        total_valid += valid_blocks[i];
        total_invalid += invalid_blocks[i];
        total_credits += credits[i];
        
        if (valid_blocks[i] + invalid_blocks[i] > 0) {
            total_time += avg_validation_time[i] * (valid_blocks[i] + invalid_blocks[i]);
            total_blocks += (valid_blocks[i] + invalid_blocks[i]);
        }
    }
    
    // Linha em branco
    strcpy(messages[msg_idx++], "");
    
    // Estatísticas totais
    strcpy(messages[msg_idx++], "Total Statistics:");
    snprintf(messages[msg_idx++], 1024, "  Total valid blocks: %d", total_valid);
    snprintf(messages[msg_idx++], 1024, "  Total invalid blocks: %d", total_invalid);
    snprintf(messages[msg_idx++], 1024, "  Total blocks: %d", total_valid + total_invalid);
    snprintf(messages[msg_idx++], 1024, "  Total credits: %d", total_credits);
    
    if (total_blocks > 0) {
        snprintf(messages[msg_idx++], 1024, "  Overall avg. validation time: %.6f seconds", total_time / total_blocks);
    }
    
    strcpy(messages[msg_idx++], "================================");
    strcpy(messages[msg_idx++], "");
    
    // Enviar todas as mensagens atomicamente
    log_atomic_messages(messages, msg_idx);
    
    // Libera a memória alocada
    for (int i = 0; i < MAX_STATS_LINES; i++) {
        free(messages[i]);
    }
    free(messages);
}

// Função principal do Statistics
int statistics_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id) {
    // Registra o handler de sinais
    signal(SIGTERM, statistics_signal_handler);
    signal(SIGINT, statistics_signal_handler);
    signal(SIGUSR1, statistics_signal_handler);
    
    // Obtém acesso à memória compartilhada
    TransactionPool *transaction_pool = (TransactionPool *)shmat(transaction_pool_id, NULL, 0);
    if (transaction_pool == (void *)-1) {
        perror("shmat TransactionPool");
        return EXIT_FAILURE;
    }
    
    BlockchainLedger *blockchain_ledger = (BlockchainLedger *)shmat(blockchain_ledger_id, NULL, 0);
    if (blockchain_ledger == (void *)-1) {
        perror("shmat BlockchainLedger");
        return EXIT_FAILURE;
    }
    
    Config *config = (Config *)shmat(config_shm_id, NULL, 0);
    if (config == (void *)-1) {
        perror("shmat Config");
        return EXIT_FAILURE;
    }
    
    // Aloca memória para armazenar estatísticas por minerador
    int *valid_blocks = (int *)calloc(config->num_miners, sizeof(int));
    int *invalid_blocks = (int *)calloc(config->num_miners, sizeof(int));
    float *avg_validation_time = (float *)calloc(config->num_miners, sizeof(float));
    int *validation_count = (int *)calloc(config->num_miners, sizeof(int));
    int *credits = (int *)calloc(config->num_miners, sizeof(int));
    
    if (!valid_blocks || !invalid_blocks || !avg_validation_time || !validation_count || !credits) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    
    // Loop principal do Statistics
    while (!should_terminate) {
        // Verifica se SIGUSR1 foi recebido
        if (sigusr1_received_statistics) {
            log_message("STATISTICS: Printing statistics due to SIGUSR1");
            print_statistics(valid_blocks, invalid_blocks, avg_validation_time, credits, config->num_miners);
            sigusr1_received_statistics = 0; // Reinicia a flag
        }

        // Recebe mensagens de estatísticas (modo bloqueante)
        StatisticsMessage msg;
        ssize_t ret = msgrcv(msg_queue_id, &msg, sizeof(StatisticsMessage) - sizeof(long), 0, 0);
        if (ret == -1) {
            if (should_terminate) {
                break;
            }
            // Se msgrcv for interrompido por sinal, apenas continua o loop
            continue;
        }

            // Processa a mensagem
            int miner_idx = msg.miner_id - 1; // IDs começam em 1
            if (miner_idx >= 0 && miner_idx < config->num_miners) {
                // Atualiza estatísticas
                if (msg.valid) {
                    valid_blocks[miner_idx]++;
                    credits[miner_idx] += msg.total_rewards; // Use total_rewards para créditos
                } else {
                    invalid_blocks[miner_idx]++;
                }
                // Atualiza o tempo médio de validação
                float current_total = avg_validation_time[miner_idx] * validation_count[miner_idx];
                validation_count[miner_idx]++;
                avg_validation_time[miner_idx] = (current_total + msg.elapsed_time) / validation_count[miner_idx];
            }
    }
    
    // Exibe estatísticas finais
    log_message("STATISTICS: Printing final statistics before exiting");
    print_statistics(valid_blocks, invalid_blocks, avg_validation_time, credits, config->num_miners);
    
    // Libera a memória
    free(valid_blocks);
    free(invalid_blocks);
    free(avg_validation_time);
    free(validation_count);
    free(credits);
    
    // Desanexa a memória compartilhada
    if (shmdt(transaction_pool) == -1) {
        perror("shmdt TransactionPool");
    }
    
    if (shmdt(blockchain_ledger) == -1) {
        perror("shmdt BlockchainLedger");
    }
    
    if (shmdt(config) == -1) {
        perror("shmdt Config");
    }
    
    log_message("STATISTICS: Processo encerrado");
    
    return EXIT_SUCCESS;
} 