#include "txgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>

// Contador de transações
static int transaction_counter = 0;

// Função para gerar uma transação aleatória
Transaction generate_transaction(int reward) {
    Transaction tx;
    
    // Gera um ID único para a transação
    snprintf(tx.tx_id, TX_ID_LEN, "TX-%d-%d", getpid(), transaction_counter++);
    
    // Define a recompensa (entre 1 e 3)
    tx.reward = (reward >= 1 && reward <= 3) ? reward : (1 + (rand() % 3));
    
    // Gera um valor aleatório para a transação (entre 0 e 100)
    tx.value = ((float)rand() / RAND_MAX) * 100.0;
    
    // Define o timestamp atual
    tx.timestamp = time(NULL);
    
    return tx;
}

// Função para adicionar uma transação ao pool
int add_transaction_to_pool(Transaction tx, TransactionPool *pool, int pool_size) {
    // Adquire o semáforo do pool
    sem_wait(&pool->mutex);
    
    // Procura uma posição vazia no pool
    int position = -1;
    for (int i = 0; i < pool_size; i++) {
        if (pool->transactions_list[i].empty) {
            position = i;
            break;
        }
    }
    
    // Se encontrou uma posição vazia
    if (position != -1) {
        // Antes de adicionar, verifica se já existe uma transação igual (evita duplicatas)
        int duplicated = 0;
        for (int i = 0; i < pool_size; i++) {
            if (i != position && !pool->transactions_list[i].empty && 
                strcmp(pool->transactions_list[i].transaction.tx_id, tx.tx_id) == 0) {
                duplicated = 1;
                break;
            }
        }
        
        if (!duplicated) {
            // Adiciona a transação ao pool
            pool->transactions_list[position].transaction = tx;
            pool->transactions_list[position].empty = 0;
            pool->transactions_list[position].age = 0;
            
            // Atualiza a ocupação do pool
            int count = 0;
            for (int i = 0; i < pool_size; i++) {
                if (!pool->transactions_list[i].empty) {
                    count++;
                }
            }
            
            pool->occupancy = (count * 100) / pool_size;
            
            // Desbloqueia o semáforo do pool ANTES de notificar
            sem_post(&pool->mutex);
            // Notifica um minerador que uma nova transação foi adicionada
            sem_post(&pool->work_notification_sem); 
            return 0;
        } else {
            // Transação duplicada
            sem_post(&pool->mutex);
            return -2; // Código para transação duplicada
        }
    }
    
    // Desbloqueia o semáforo do pool se não adicionou
    sem_post(&pool->mutex);
    return -1; // Pool cheio
}

// Função principal do Transaction Generator
int txgen_main(int transaction_pool_id, int config_shm_id, int reward, int sleep_time) {
    // Inicializa o gerador de números aleatórios
    srand(time(NULL) + getpid());
    
    // Obtém acesso à memória compartilhada
    TransactionPool *transaction_pool = (TransactionPool *)shmat(transaction_pool_id, NULL, 0);
    if (transaction_pool == (void *)-1) {
        perror("shmat TransactionPool");
        return EXIT_FAILURE;
    }
    
    Config *config = (Config *)shmat(config_shm_id, NULL, 0);
    if (config == (void *)-1) {
        perror("shmat Config");
        return EXIT_FAILURE;
    }
    
    printf("Transaction Generator started (PID: %d)\n", getpid());
    printf("Reward: %d, Sleep time: %d ms\n", reward, sleep_time);
    
    // Loop para gerar transações
    while (1) {
        // Gera uma transação
        Transaction tx = generate_transaction(reward);
        
        // Adiciona ao pool
        int result = add_transaction_to_pool(tx, transaction_pool, config->tx_pool_size);
        if (result == 0) {
            printf("Transaction generated: ID=%s, Reward=%d, Value=%.2f\n", 
                  tx.tx_id, tx.reward, tx.value);
        } else if (result == -1) {
            printf("Transaction pool is full, could not add transaction\n");
        } else if (result == -2) {
            printf("Duplicate transaction ID=%s, skipping\n", tx.tx_id);
        }
        
        // Aguarda o intervalo especificado
        usleep(sleep_time * 1000); // Converte ms para µs
    }
    
    // Desanexa a memória compartilhada
    if (shmdt(transaction_pool) == -1) {
        perror("shmdt TransactionPool");
    }
    
    if (shmdt(config) == -1) {
        perror("shmdt Config");
    }
    
    return EXIT_SUCCESS;
}

// Função principal do TxGen
int main(int argc, char *argv[]) {
    // Verifica os argumentos
    if (argc != 3) {
        printf("Usage: %s <reward> <sleep_time_ms>\n", argv[0]);
        printf("  reward: 1-3 (difficulty level)\n");
        printf("  sleep_time_ms: 200-3000 (milliseconds between transactions)\n");
        return EXIT_FAILURE;
    }
    
    // Obtém os argumentos
    int reward = atoi(argv[1]);
    int sleep_time = atoi(argv[2]);
    
    // Valida os argumentos
    if (reward < 1 || reward > 3) {
        printf("Invalid reward (should be 1-3)\n");
        return EXIT_FAILURE;
    }
    
    if (sleep_time < 200 || sleep_time > 3000) {
        printf("Invalid sleep time (should be 200-3000 ms)\n");
        return EXIT_FAILURE;
    }
    
    // Obtém o ID do transaction pool e da config
    key_t key_tx = ftok(".", 'T');
    int transaction_pool_id = shmget(key_tx, 0, 0);
    if (transaction_pool_id == -1) {
        perror("shmget TransactionPool");
        return EXIT_FAILURE;
    }
    
    key_t key_cfg = ftok(".", 'C');
    int config_shm_id = shmget(key_cfg, 0, 0);
    if (config_shm_id == -1) {
        perror("shmget Config");
        return EXIT_FAILURE;
    }
    
    // Inicia o transaction generator
    return txgen_main(transaction_pool_id, config_shm_id, reward, sleep_time);
} 