#include "miner.h"
#include "logger.h"
#include "pow.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

// Flag para terminar
static int should_terminate = 0;

// Handler para sinais
void miner_signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        should_terminate = 1;
    }
}

// Função para selecionar transações da pool para um novo bloco
Transaction *select_transactions(TransactionPool *pool, int transactions_per_block, int miner_id, Config *config) {
    Transaction *selected = (Transaction *)malloc(transactions_per_block * sizeof(Transaction));
    if (!selected) {
        perror("malloc");
        return NULL;
    }
    
    int count = 0;
    int *selected_indices = (int *)malloc(transactions_per_block * sizeof(int));
    
    if (!selected_indices) {
        free(selected);
        perror("malloc");
        return NULL;
    }
    
    // Adquire o semáforo da pool
    sem_wait(&pool->mutex);
    
    // Verifica se há transações suficientes
    int available_transactions = 0;
    for (int i = 0; i < config->tx_pool_size; i++) {
        if (!pool->transactions_list[i].empty) {
            available_transactions++;
        }
    }
    
    // Se não há transações suficientes, libera a memória e retorna NULL
    if (available_transactions < transactions_per_block) {
        sem_post(&pool->mutex);
        free(selected);
        free(selected_indices);
        return NULL;
    }
    
    // Seleciona transações e marca as posições
    for (int i = 0, j = 0; i < config->tx_pool_size && count < transactions_per_block; i++) {
        if (!pool->transactions_list[i].empty) {
            // Verifica se esta transação já foi incluída (proteção extra)
            int already_selected = 0;
            for (j = 0; j < count; j++) {
                if (strcmp(pool->transactions_list[i].transaction.tx_id, 
                           selected[j].tx_id) == 0) {
                    already_selected = 1;
                    break;
                }
            }
            
            if (!already_selected) {
                // Copia a transação
                memcpy(&selected[count], &pool->transactions_list[i].transaction, sizeof(Transaction));
                // Marca o índice para uso posterior
                selected_indices[count] = i;
                // Temporariamente marca a transação como reservada definindo empty=1
                pool->transactions_list[i].empty = 1; 
                count++;
            }
        }
    }
    
    // Libera o semáforo
    sem_post(&pool->mutex);
    
    // Registra quais transações foram usadas
    if (count == transactions_per_block) {
        char debug_msg[200];
        snprintf(debug_msg, sizeof(debug_msg), "MINER %d: Reserved transactions for mining block", miner_id);
        log_message(debug_msg);
    } else {
        // Se não conseguiu transações suficientes, desfaz as reservas
        sem_wait(&pool->mutex);
        for (int i = 0; i < count; i++) {
            // Restaura as transações ao pool
            pool->transactions_list[selected_indices[i]].empty = 0;
        }
        sem_post(&pool->mutex);
        
        free(selected);
        free(selected_indices);
        selected = NULL;
    }
    
    free(selected_indices);
    return selected;
}

// Função para criar um novo bloco
TransactionBlock *create_block(Transaction *transactions, int transactions_per_block, char *previous_hash, int miner_id, int block_id) {
    // Use calloc para garantir que a estrutura do bloco é inicializada a zero
    TransactionBlock *block = (TransactionBlock *)calloc(1, sizeof(TransactionBlock));
    if (!block) {
        perror("calloc block");
        return NULL;
    }
    
    // Inicializa o bloco
    snprintf(block->txb_id, TXB_ID_LEN, "BLOCK-%d-%d", miner_id, block_id);
    memcpy(block->previous_block_hash, previous_hash, HASH_SIZE);
    block->timestamp = time(NULL);
    block->txb_id[TXB_ID_LEN - 1] = '\0';
    block->previous_block_hash[HASH_SIZE - 1] = '\0'; 
    block->transactions = transactions;
    block->nonce = 0;
    block->transactions_per_block = transactions_per_block;
    
    return block;
}

// Função para enviar um bloco para o validador através do named pipe
int send_block_to_validator(TransactionBlock *block, int miner_id, volatile int *should_terminate_flag) {
    int fd = -1;
    fd = open(VALIDATOR_INPUT, O_WRONLY);
    if (fd == -1) {
        if (errno == EINTR && *should_terminate_flag) {
            log_miner_message(miner_id, "Open validator pipe interrupted, terminating.");
            return -1;
        }
        // Se *should_terminate_flag for verdadeiro, o erro de pipe é secundário.
        log_miner_message(miner_id, "Failed to open validator pipe: %s", strerror(errno));
        return -1;
    }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    if ((size_t)write(fd, &miner_id, sizeof(int)) != sizeof(int)) {
        if (!(*should_terminate_flag && errno == EPIPE)) perror("write miner_id");
        close(fd); return -1;
    }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    if ((size_t)write(fd, block->txb_id, TXB_ID_LEN) != TXB_ID_LEN) { if (!(*should_terminate_flag && errno == EPIPE)) perror("write txb_id"); close(fd); return -1; }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    if ((size_t)write(fd, block->previous_block_hash, HASH_SIZE) != HASH_SIZE) { if (!(*should_terminate_flag && errno == EPIPE)) perror("write previous_hash"); close(fd); return -1; }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    if ((size_t)write(fd, &block->timestamp, sizeof(time_t)) != sizeof(time_t)) { if (!(*should_terminate_flag && errno == EPIPE)) perror("write timestamp"); close(fd); return -1; }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    if ((size_t)write(fd, &block->nonce, sizeof(unsigned int)) != sizeof(unsigned int)) { if (!(*should_terminate_flag && errno == EPIPE)) perror("write nonce"); close(fd); return -1; }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    if ((size_t)write(fd, &block->transactions_per_block, sizeof(int)) != sizeof(int)) { if (!(*should_terminate_flag && errno == EPIPE)) perror("write tx_per_block"); close(fd); return -1; }
    
    if (*should_terminate_flag) { close(fd); return -1; }
    size_t transactions_size = block->transactions_per_block * sizeof(Transaction);
    if ((size_t)write(fd, block->transactions, transactions_size) != transactions_size) {
        if (!(*should_terminate_flag && errno == EPIPE)) perror("write transactions"); // EPIPE (Broken pipe) é esperado se o validador saiu
        close(fd); return -1;
    }
    
    close(fd);
    return 0;
}

// Função executada por cada thread Miner
void *miner_thread_function(void *arg) {
    MinerThreadArgs *args = (MinerThreadArgs *)arg;
    int miner_id = args->id;
    TransactionPool *pool = args->transaction_pool;
    BlockchainLedger *ledger = args->blockchain_ledger;
    Config *config = args->config;
    volatile int *should_terminate_ptr = args->should_terminate;
    
    log_message("MINER: THREAD MINER %d CREATED", miner_id);
    
    // Configura o gerador de números aleatórios com seed diferente para cada thread
    srand(time(NULL) + miner_id);
    
    // Loop principal do Miner
    while (*should_terminate_ptr == 0) {
        // Espera por uma notificação de que transações foram adicionadas
        if (sem_wait(&pool->work_notification_sem) == -1) {
            if (errno == EINTR && *should_terminate_ptr) { // Interrompido por sinal e deve terminar
                 log_miner_message(miner_id, "sem_wait interrupted, terminating.");
                 break;
            }
            // Se o sem_wait falhar por outro motivo ou se não for para terminar
            perror("sem_wait work_notification_sem in miner thread"); 
            break; 
        }

        if (*should_terminate_ptr) break;

        // Obtém o hash do último bloco válido (usa ledger->mutex)
        char previous_hash[HASH_SIZE];
        sem_wait(&ledger->mutex);
        if (ledger->num_blocks > 0) {
            // Calcula o hash do último bloco
            compute_sha256(&ledger->blocks[ledger->num_blocks - 1].block, previous_hash);
        } else {
            // Utiliza o hash inicial para o primeiro bloco
            strcpy(previous_hash, INITIAL_HASH);
        }
        // Obtém o ID do próximo bloco potencial (o ledger->num_blocks atual antes de adicionar um novo)
        int block_id = ledger->num_blocks;
        sem_post(&ledger->mutex);
        
        if (*should_terminate_ptr) break;
        
        // Seleciona transações. pool->mutex internamente.
        Transaction *transactions = select_transactions(pool, config->transactions_per_block, miner_id, config);
        
        if (!transactions) {
            if (*should_terminate_ptr) break;
            continue; 
        }
        
        if (*should_terminate_ptr) { free(transactions); break; }

        // NOVO: Verifica se todas as transações selecionadas ainda são válidas
        sem_wait(&pool->mutex);
        int all_valid = 1;
        char *tx_ids_to_check[config->transactions_per_block];
        
        // Salva os IDs das transações para verificar
        for (int i = 0; i < config->transactions_per_block; i++) {
            tx_ids_to_check[i] = strdup(transactions[i].tx_id);
            if (tx_ids_to_check[i] == NULL) {
                perror("strdup");
                all_valid = 0;
                break;
            }
        }
        sem_post(&pool->mutex);
        
        // Se não conseguiu alocar memória para os IDs, libera e continua
        if (!all_valid) {
            // Restaura as transações ao pool
            sem_wait(&pool->mutex);
            for (int i = 0; i < config->tx_pool_size; i++) {
                for (int j = 0; j < config->transactions_per_block; j++) {
                    if (strcmp(pool->transactions_list[i].transaction.tx_id, transactions[j].tx_id) == 0) {
                        pool->transactions_list[i].empty = 0;
                        break;
                    }
                }
            }
            sem_post(&pool->mutex);
            
            // Libera memória
            for (int i = 0; i < config->transactions_per_block && tx_ids_to_check[i] != NULL; i++) {
                free(tx_ids_to_check[i]);
            }
            free(transactions);
            continue;
        }

        log_miner_message(miner_id, "STARTED MINING BLOCK (transactions selected)");
        
        // Cria o bloco
        TransactionBlock *block = create_block(transactions, config->transactions_per_block, previous_hash, miner_id, block_id);
        if (!block) {
            // Em caso de falha, devolvemos as transações ao pool
            log_miner_message(miner_id, "FAILED TO CREATE BLOCK, RETURNING TRANSACTIONS TO POOL");
            
            // Restaura as transações ao pool, pois não usamos
            sem_wait(&pool->mutex);
            for (int i = 0; i < config->tx_pool_size; i++) {
                for (int j = 0; j < config->transactions_per_block; j++) {
                    if (strcmp(pool->transactions_list[i].transaction.tx_id, transactions[j].tx_id) == 0) {
                        pool->transactions_list[i].empty = 0;
                        break;
                    }
                }
            }
            sem_post(&pool->mutex);
            
            // Libera memória
            for (int i = 0; i < config->transactions_per_block; i++) {
                free(tx_ids_to_check[i]);
            }
            free(transactions);
            continue;
        }
        
        if (*should_terminate_ptr) { 
            for (int i = 0; i < config->transactions_per_block; i++) {
                free(tx_ids_to_check[i]);
            }
            free(transactions); 
            free(block); 
            break; 
        }
        
        // Realiza o Proof of Work
        log_miner_message(miner_id, "CALCULATING PROOF OF WORK");
        PoWResult pow_result = proof_of_work(block);
        
        if (*should_terminate_ptr) { 
            for (int i = 0; i < config->transactions_per_block; i++) {
                free(tx_ids_to_check[i]);
            }
            free(transactions); 
            free(block); 
            break; 
        }

        if (pow_result.error) {
            // Falha no PoW, tenta novamente com outro timestamp
            log_miner_message(miner_id, "PROOF OF WORK FAILED, RETRYING");
            block->timestamp = time(NULL);
            pow_result = proof_of_work(block);
            
            if (*should_terminate_ptr) { 
                for (int i = 0; i < config->transactions_per_block; i++) {
                    free(tx_ids_to_check[i]);
                }
                free(transactions); 
                free(block); 
                break; 
            }

            if (pow_result.error) {
                // Ainda falhou, desiste deste bloco
                log_miner_message(miner_id, "PROOF OF WORK FAILED AGAIN, GIVING UP");
                
                // Restaura as transações ao pool, pois não usamos
                sem_wait(&pool->mutex);
                for (int i = 0; i < config->tx_pool_size; i++) {
                    for (int j = 0; j < config->transactions_per_block; j++) {
                        if (strcmp(pool->transactions_list[i].transaction.tx_id, transactions[j].tx_id) == 0) {
                            pool->transactions_list[i].empty = 0;
                            break;
                        }
                    }
                }
                sem_post(&pool->mutex);
                
                for (int i = 0; i < config->transactions_per_block; i++) {
                    free(tx_ids_to_check[i]);
                }
                free(transactions);
                free(block);
                continue;
            }
        }
        
        // PoW bem-sucedido, envia o bloco para o validador
        log_miner_message(miner_id, "FINISHED MINING BLOCK (NONCE=%u, HASH=%s)", block->nonce, pow_result.hash);
        
        if (*should_terminate_ptr) { 
            for (int i = 0; i < config->transactions_per_block; i++) {
                free(tx_ids_to_check[i]);
            }
            free(transactions); 
            free(block); 
            break; 
        }

        if (send_block_to_validator(block, miner_id, should_terminate_ptr) == -1) {
            log_miner_message(miner_id, "FAILED TO SEND BLOCK TO VALIDATOR, RETURNING TRANSACTIONS TO POOL");
            
            // Restaura as transações ao pool, pois falhou o envio
            sem_wait(&pool->mutex);
            for (int i = 0; i < config->tx_pool_size; i++) {
                for (int j = 0; j < config->transactions_per_block; j++) {
                    if (strcmp(pool->transactions_list[i].transaction.tx_id, transactions[j].tx_id) == 0) {
                        pool->transactions_list[i].empty = 0;
                        break;
                    }
                }
            }
            sem_post(&pool->mutex);
        }
        
        // Libera memória
        for (int i = 0; i < config->transactions_per_block; i++) {
            free(tx_ids_to_check[i]);
        }
        
        // Libera memória para o bloco e suas transações após o envio (ou falha no envio)
        if (block != NULL) {
             if (block->transactions != NULL) {
                 free(block->transactions); // Libera o array de transações alocado por select_transactions
             }
             free(block); // Libera a estrutura do bloco alocada por create_block
        }
        block = NULL; transactions = NULL; // Evitar double free ou uso após free na próxima iteração se algo correr mal
    }
    
    log_message("MINER %d: Thread encerrado", miner_id);
    
    pthread_exit(NULL);
}

// Função principal do Miner
int miner_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id __attribute__((unused)), int config_shm_id) {
    pthread_t *miner_threads;
    MinerThreadArgs *thread_args;
    
    // Registra o handler de sinais
    signal(SIGTERM, miner_signal_handler);
    signal(SIGINT, miner_signal_handler);
    
    // Obtém acesso à memória compartilhada do pool de transações
    TransactionPool *transaction_pool = (TransactionPool *)shmat(transaction_pool_id, NULL, 0);
    if (transaction_pool == (void *)-1) {
        perror("shmat TransactionPool");
        return EXIT_FAILURE;
    }
    
    // Obtém acesso à memória compartilhada do blockchain ledger
    BlockchainLedger *blockchain_ledger = (BlockchainLedger *)shmat(blockchain_ledger_id, NULL, 0);
    if (blockchain_ledger == (void *)-1) {
        perror("shmat BlockchainLedger");
        return EXIT_FAILURE;
    }
    
    // Obtém acesso à memória compartilhada da Config
    Config *config = (Config *)shmat(config_shm_id, NULL, 0);
    if (config == (void *)-1) {
        perror("shmat Config");
        return EXIT_FAILURE;
    }
    
    // Aloca memória para os threads e argumentos
    miner_threads = (pthread_t *)malloc(config->num_miners * sizeof(pthread_t));
    thread_args = (MinerThreadArgs *)malloc(config->num_miners * sizeof(MinerThreadArgs));
    
    if (miner_threads == NULL || thread_args == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    
    // Inicializa os argumentos e cria os threads
    for (int i = 0; i < config->num_miners; i++) {
        thread_args[i].id = i + 1; // IDs começam em 1
        thread_args[i].transaction_pool = transaction_pool;
        thread_args[i].blockchain_ledger = blockchain_ledger;
        thread_args[i].config = config;
        thread_args[i].should_terminate = &should_terminate;
        
        if (pthread_create(&miner_threads[i], NULL, miner_thread_function, &thread_args[i]) != 0) {
            perror("pthread_create");
            should_terminate = 1;
            break;
        }
    }
    
    // Aguarda o sinal de término
    while (!should_terminate) {
        pause();
    }

    // Notifica todas as threads miners para acordarem e verificarem a flag para terminar
    if (config != NULL && transaction_pool != NULL) { // Verificar se config e pool são válidos
        for (int i = 0; i < config->num_miners; i++) {
            sem_post(&transaction_pool->work_notification_sem);
        }
    }

    // Encerra todos os threads
    for (int i = 0; i < config->num_miners; i++) {
        pthread_join(miner_threads[i], NULL);
    }
    
    // Libera a memória
    free(miner_threads);
    free(thread_args);
    
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
    
    log_message("MINER: Processo encerrado");
    
    return EXIT_SUCCESS;
} 