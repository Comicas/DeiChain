#include "validator.h"
#include "logger.h"
#include "pow.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/select.h>
#include <sys/time.h>

// Flag para terminar
static int should_terminate = 0;

// Contador de validators
static int validator_id = 1;
static int active_validators = 1;

// Timestamp da última criação de validator para prevenir criações múltiplas
static time_t last_validator_creation = 0;

// Timestamp da última vez que as idades das transações foram incrementadas
static time_t last_age_update = 0;

// Handler para sinais
void validator_signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        should_terminate = 1;
    }
}

// Função para verificar a validade de um bloco
int validate_block(TransactionBlock *block, BlockchainLedger *ledger, TransactionPool *pool, Config *config) {
    char hash[HASH_SIZE];
    
    // Computa o hash do bloco
    compute_sha256(block, hash);
    log_message("VALIDATOR: Checking block with hash: %s", hash);
    
    // Verifica o nonce (Proof of Work)
    if (!verify_nonce(block)) {
        log_message("VALIDATOR: Block invalid - PoW verification failed");
        return 0; // Bloco inválido - PoW falhou
    }
    
    // Verifica o hash do bloco anterior
    sem_wait(&ledger->mutex);
    
    if (ledger->num_blocks > 0) {
        // Compara com o hash do último bloco válido
        char last_hash[HASH_SIZE];
        compute_sha256(&ledger->blocks[ledger->num_blocks - 1].block, last_hash);
        
        if (strcmp(block->previous_block_hash, last_hash) != 0) {
            log_message("VALIDATOR: Block invalid - Previous hash mismatch");
            log_message("VALIDATOR: Expected: %s, Got: %s", last_hash, block->previous_block_hash);
            sem_post(&ledger->mutex);
            return 0; // Bloco inválido - hash anterior incorreto
        }
    } else {
        // Para o primeiro bloco, verifica se ele usa o hash inicial
        if (strcmp(block->previous_block_hash, INITIAL_HASH) != 0) {
            log_message("VALIDATOR: Block invalid - Initial block should reference INITIAL_HASH");
            sem_post(&ledger->mutex);
            return 0;
        }
    }
    
    sem_post(&ledger->mutex);
    
    // Verifica se as transações existem no pool
    sem_wait(&pool->mutex);
    
    int valid_transactions = 1;
    int transactions_found = 0;
    int *tx_positions = (int *)malloc(block->transactions_per_block * sizeof(int));
    if (!tx_positions) {
        log_message("VALIDATOR: Failed to allocate memory for transaction positions");
        sem_post(&pool->mutex);
        return 0;
    }
    
    // Inicializa o array de posições com -1 (não encontrado)
    for (int i = 0; i < block->transactions_per_block; i++) {
        tx_positions[i] = -1;
    }
    
    // Verifica se há transações válidas
    for (int i = 0; i < block->transactions_per_block; i++) {
        if (strlen(block->transactions[i].tx_id) == 0) {
            log_message("VALIDATOR: Block invalid - Transaction %d has empty tx_id", i);
            valid_transactions = 0;
            break;
        }
        
        // Procura a transação no pool
        int found = 0;
        for (int j = 0; j < config->tx_pool_size; j++) {
            // Verifica tanto em transações disponíveis quanto reservadas (empty=1)
            if (strcmp(block->transactions[i].tx_id, pool->transactions_list[j].transaction.tx_id) == 0) {
                found = 1;
                tx_positions[i] = j;
                transactions_found++;
                break;
            }
        }
        
        if (!found) {
            log_message("VALIDATOR: Transaction %s not found in pool", block->transactions[i].tx_id);
            valid_transactions = 0;
            break;
        }
    }
    
    // Se todas as transações são válidas, marca-as como consumidas
    if (valid_transactions) {
        // Mantem as transações marcadas como empty=1 (reservadas)
        // Elas serão removidas efetivamente após a adição do bloco ao ledger
    } else {
        // Devolve as transações encontradas para o pool (desmarca a reserva)
        for (int i = 0; i < block->transactions_per_block; i++) {
            if (tx_positions[i] != -1) {
                // Se a transação foi encontrada (reservada), desmarca a reserva
                pool->transactions_list[tx_positions[i]].empty = 0;
                log_message("VALIDATOR: Returned transaction %s to pool", block->transactions[i].tx_id);
            }
        }
    }
    
    free(tx_positions);
    sem_post(&pool->mutex);
    
    if (!valid_transactions) {
        log_message("VALIDATOR: Block invalid - Only %d of %d transactions found in pool", 
                   transactions_found, block->transactions_per_block);
        return 0;
    }
    
    log_message("VALIDATOR: Block validation successful");
    return 1; // Bloco válido
}

// Função para adicionar um bloco validado ao ledger
int add_block_to_ledger(TransactionBlock *block, BlockchainLedger *ledger, Config *config) {
    // Verifica se há espaço no ledger
    if (ledger->num_blocks >= config->blockchain_blocks) {
        return -1; // Ledger cheio
    }
    
    // Adquire o semáforo do ledger
    sem_wait(&ledger->mutex);
    
    // Adiciona o bloco ao ledger
    BlockchainEntry *entry_ptr = &ledger->blocks[ledger->num_blocks];

    // Calcula a localização das transações para o bloco 'i' dentro do segmento de memória compartilhada.
    // Endereço base + tamanho da parte fixa do Ledger + tamanho de todas as estruturas Entry + deslocamento para as transações deste bloco
    void *entries_base = (void *)ledger->blocks; // Endereço base da parte flexível do array de blocos
    void *transactions_base = entries_base + (config->blockchain_blocks * sizeof(BlockchainEntry)); // Endereço base das transações
    size_t block_tx_offset = ledger->num_blocks * config->transactions_per_block * sizeof(Transaction);
    Transaction *target_tx_location = (Transaction *)(transactions_base + block_tx_offset);

    // Copia a estrutura principal do bloco
    memcpy(&entry_ptr->block, block, sizeof(TransactionBlock));
    
    // Copia as transações para a localização calculada na memória compartilhada
    memcpy(target_tx_location, 
           block->transactions, 
           config->transactions_per_block * sizeof(Transaction));

    // Atualiza o ponteiro de transações no bloco dentro da memória compartilhada
    entry_ptr->block.transactions = target_tx_location;
    
    // Marca o bloco como válido
    ledger->blocks[ledger->num_blocks].valid = 1;
    
    // Incrementa o contador de blocos
    ledger->num_blocks++;
    
    // Libera o semáforo
    sem_post(&ledger->mutex);
    
    return 0;
}

// Função para remover transações validadas do pool
void remove_transactions_from_pool(TransactionBlock *block, TransactionPool *pool, Config *config) {
    // Adquire o semáforo do pool
    sem_wait(&pool->mutex);
    
    // Para cada transação no bloco
    for (int i = 0; i < config->transactions_per_block; i++) {
        // Procura a transação no pool
        for (int j = 0; j < config->tx_pool_size; j++) {
            if (strcmp(pool->transactions_list[j].transaction.tx_id, block->transactions[i].tx_id) == 0) {
                // Marca a posição como vazia definitivamente
                pool->transactions_list[j].empty = 1;
                pool->transactions_list[j].age = 0;
                break;
            }
        }
    }
    
    // Atualiza a ocupação do pool
    int count = 0;
    for (int i = 0; i < config->tx_pool_size; i++) {
        if (!pool->transactions_list[i].empty) {
            count++;
        }
    }
    
    pool->occupancy = (count * 100) / config->tx_pool_size;
    
    // Libera o semáforo
    sem_post(&pool->mutex);
}

// Função para incrementar a idade das transações no pool
void age_transaction_pool(TransactionPool *pool, Config *config) {
    // Verifica se passou pelo menos 1 segundo desde a última atualização
    time_t current_time = time(NULL);
    if (current_time <= last_age_update) {
        return; // Ainda não é hora de incrementar novamente
    }
    
    // Adquire o semáforo do pool
    sem_wait(&pool->mutex);
    
    // Incrementa a idade de cada transação
    for (int i = 0; i < config->tx_pool_size; i++) {
        if (!pool->transactions_list[i].empty) {
            pool->transactions_list[i].age++;
            
            // Se a idade for múltiplo de 50, incrementa a recompensa
            if (pool->transactions_list[i].age % 50 == 0) {
                pool->transactions_list[i].transaction.reward++;
            }
        }
    }
    
    // Libera o semáforo
    sem_post(&pool->mutex);
    
    // Atualiza o timestamp da última atualização
    last_age_update = current_time;
}

// Função para gerenciar os validadores dinâmicos baseados na ocupação do pool
void manage_validators(TransactionPool *pool, int *active_validators) {
    // Apenas o validator principal (id=1) deve gerenciar outros validadores
    if (validator_id != 1) {
        return; // Os validadores filhos não devem criar outros validadores
    }
    
    // Verificar se passou tempo suficiente desde a última criação (5 segundos)
    time_t current_time = time(NULL);
    if (current_time - last_validator_creation < 5) {
        return; // Não cria um novo validador se o último foi criado há menos de 5 segundos
    }
    
    // Adquire o semáforo do pool
    sem_wait(&pool->mutex);
    
    int occupancy = pool->occupancy;
    
    // Libera o semáforo
    sem_post(&pool->mutex);
    
    // Decide se deve criar ou remover validadores
    if (occupancy >= 80 && *active_validators < 3) {
        // Cria um terceiro validador (quando a ocupação atinge 80%)
        pid_t pid = fork();
        
        if (pid == 0) {
            // Processo filho (novo validador)
            validator_id = 3; // Definindo ID explicitamente
            log_validator_message(validator_id, "TRANSACTIONS POOL %d%% FULL", occupancy);
            log_validator_message(validator_id, "READY FOR WORK");
            *active_validators = 1; // Reinicia o contador no filho
            return;
        } else if (pid > 0) {
            // Processo pai
            (*active_validators)++;
            last_validator_creation = current_time; // Atualiza o timestamp de criação
            log_validator_message(1, "NEW VALIDATOR CREATED");
        }
    } else if (occupancy >= 60 && occupancy < 80 && *active_validators < 2) {
        // Cria um segundo validador (quando a ocupação atinge 60%)
        pid_t pid = fork();
        
        if (pid == 0) {
            // Processo filho (novo validador)
            validator_id = 2; // Definindo ID explicitamente
            log_validator_message(validator_id, "TRANSACTIONS POOL %d%% FULL", occupancy);
            log_validator_message(validator_id, "READY FOR WORK");
            *active_validators = 1; // Reinicia o contador no filho
            return;
        } else if (pid > 0) {
            // Processo pai
            (*active_validators)++;
            last_validator_creation = current_time; // Atualiza o timestamp de criação
            log_validator_message(1, "NEW VALIDATOR CREATED");
        }
    // } else if (occupancy <= 40 && *active_validators > 1) {
    //     (*active_validators)--; // Decrementa o contador no pai
    }
}

// Função para enviar estatísticas para o processo Statistics
void send_statistics(int msg_queue_id, int miner_id, int valid, TransactionBlock *block, float elapsed_time) {
    StatisticsMessage msg;
    // Inicializa a estrutura da mensagem para evitar enviar dados não inicializados
    memset(&msg, 0, sizeof(StatisticsMessage));
    
    msg.mtype = 1; // Tipo fixo para mensagens de estatísticas
    msg.miner_id = miner_id;
    msg.valid = valid;
    msg.elapsed_time = elapsed_time;
    msg.transaction_count = 0;
    msg.total_value = 0.0;
    msg.total_rewards = 0;
    
    if (valid && block) {
        msg.transaction_count = block->transactions_per_block;
        
        // Calcula o valor total das transações e a soma das recompensas
        msg.total_value = 0.0;
        msg.total_rewards = 0;
        for (int i = 0; i < block->transactions_per_block; i++) {
            msg.total_value += block->transactions[i].value;
            msg.total_rewards += block->transactions[i].reward;
        }
    }
    
    // Envia a mensagem
    if (msgsnd(msg_queue_id, &msg, sizeof(StatisticsMessage) - sizeof(long), 0) == -1) {
        perror("msgsnd");
    }
}

// Função principal do Validator
int validator_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id) {
    // Registra o handler de sinais
    signal(SIGTERM, validator_signal_handler);
    signal(SIGINT, validator_signal_handler);
    
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
    
    // Abre o pipe para receber blocos dos mineradores
    int fd = open(VALIDATOR_INPUT, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open");
        return EXIT_FAILURE;
    }
    
    time_t last_pool_log_time = 0; // Para controlar o log da ocupação da pool
    const int POOL_LOG_INTERVAL = 3; // Logar a cada 3 segundos

    // Loop principal do Validator
    while (!should_terminate) {
        // Gerencia os validadores baseados na ocupação do pool
        manage_validators(transaction_pool, &active_validators);
        
        // Child validators check if they should terminate themselves
        if (validator_id > 1) {
            sem_wait(&transaction_pool->mutex);
            int occupancy = transaction_pool->occupancy;
            sem_post(&transaction_pool->mutex);
            
            if (occupancy <= 40) { //percentagem de ocupação para terminar os validadores filhos, alterar para debug
                log_validator_message(validator_id, "TRANSACTIONS POOL %d%% FULL", occupancy);
                log_validator_message(validator_id, "TERMINATING");
                exit(EXIT_SUCCESS);
            }
        }
        
        // Incrementa a idade das transações
        age_transaction_pool(transaction_pool, config);
        
        // Utilizar select para aguardar dados com timeout
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        
        // Define um timeout curto (100ms)
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        
        int select_result = select(fd + 1, &read_fds, NULL, NULL, &timeout);
        
        // Log periódico da ocupação da pool
        time_t current_time_for_log = time(NULL);
        if (current_time_for_log - last_pool_log_time >= POOL_LOG_INTERVAL) {
            sem_wait(&transaction_pool->mutex); // Proteger acesso à occupancy
            int current_occupancy = transaction_pool->occupancy;
            sem_post(&transaction_pool->mutex);
            log_validator_message(validator_id, "Transaction Pool Occupancy: %d%%", current_occupancy);
            last_pool_log_time = current_time_for_log;
        }
        
        if (select_result == -1) {
            // Erro no select
            if (errno != EINTR) {
                perror("select");
                break;
            }
            continue;
        } else if (select_result == 0) {
            // Timeout, não há dados disponíveis
            continue;
        }
        
        // Dados disponíveis, lê
        int miner_id;
        TransactionBlock block;
        Transaction *transactions = NULL;
        ssize_t bytes_read;

        // Use um buffer temporário para ler strings para garantir a terminação nula
        char txb_id_buf[TXB_ID_LEN];
        char prev_hash_buf[HASH_SIZE];
        
        // 1. Ler ID do Miner
        bytes_read = read(fd, &miner_id, sizeof(int));
        
        if (bytes_read == sizeof(int)) {
            // ID do Miner lido com sucesso, agora ler o restante dos dados do bloco
            int read_success = 1; // Flag para rastrear o sucesso da leitura

            // Função auxiliar para realizar leitura completa (pode precisar de múltiplas chamadas)
            ssize_t read_fully(int fd, void *buf, size_t count) {
                size_t total_read = 0;
                ssize_t bytes;
                while (total_read < count) {
                    bytes = read(fd, (char*)buf + total_read, count - total_read);
                    if (bytes <= 0) {
                        if (bytes == 0 || errno != EAGAIN) {
                            // Se bytes == 0 (EOF) ou um erro diferente de EAGAIN,
                            // retorna o que foi lido até agora, ou 'bytes' se nada foi lido (para propagar -1 em erro)
                            return total_read > 0 ? (ssize_t)total_read : bytes;
                        }
                        // Aguarda um pouco antes de tentar novamente se EAGAIN (sem dados prontos)
                        struct timeval short_wait = {0, 10000}; // 10ms
                        select(0, NULL, NULL, NULL, &short_wait);
                        continue;
                    }
                    total_read += bytes;
                }
                return (ssize_t)total_read; // Cast para ssize_t para corresponder ao tipo de retorno da função
            }

            // 2. Ler dados do bloco sequencialmente com leitura completa
            if (read_success && read_fully(fd, txb_id_buf, TXB_ID_LEN) != TXB_ID_LEN) { read_success = 0; perror("read txb_id"); }
            if (read_success && read_fully(fd, prev_hash_buf, HASH_SIZE) != HASH_SIZE) { read_success = 0; perror("read previous_hash"); }
            if (read_success && read_fully(fd, &block.timestamp, sizeof(time_t)) != sizeof(time_t)) { read_success = 0; perror("read timestamp"); }
            if (read_success && read_fully(fd, &block.nonce, sizeof(unsigned int)) != sizeof(unsigned int)) { read_success = 0; perror("read nonce"); }
            if (read_success && read_fully(fd, &block.transactions_per_block, sizeof(int)) != sizeof(int)) { read_success = 0; perror("read tx_per_block"); }

            // Validar transactions_per_block para garantir que está dentro de limites razoáveis
            if (read_success && (block.transactions_per_block <= 0 || block.transactions_per_block > config->transactions_per_block)) {
                log_message("VALIDATOR: Invalid transactions_per_block count (%d), valid range is 1-%d", 
                          block.transactions_per_block, config->transactions_per_block);
                read_success = 0;
            }

            // Copia os dados de string para a estrutura do bloco
            if (read_success) {
                memcpy(block.txb_id, txb_id_buf, TXB_ID_LEN);
                memcpy(block.previous_block_hash, prev_hash_buf, HASH_SIZE);
                // Certifique-se de que a terminação nula está correta
                block.txb_id[TXB_ID_LEN - 1] = '\0'; 
                block.previous_block_hash[HASH_SIZE - 1] = '\0';
            }

            // 3. Ler dados das transações
            if (read_success) {
                 // Use o valor de transactions_per_block validado
                 size_t transactions_size = block.transactions_per_block * sizeof(Transaction);
                 transactions = (Transaction *)malloc(transactions_size);
                 if (!transactions) { 
                     perror("malloc transactions");
                     read_success = 0; 
                 } else {
                     if (read_fully(fd, transactions, transactions_size) != (ssize_t)transactions_size) {
                         perror("read transactions");
                         free(transactions);
                         transactions = NULL;
                         read_success = 0;
                     }
                 }
            }

            // Se todas as partes foram lidas com sucesso, prossegue com a validação
            if (read_success && transactions != NULL) {
                block.transactions = transactions; // Atribui o ponteiro para as transações lidas
                
                // Valida o bloco
                log_validator_message(validator_id, "STARTED VALIDATING BLOCK FROM MINER %d", miner_id);
                
                clock_t start = clock();
                int valid = validate_block(&block, blockchain_ledger, transaction_pool, config);
                clock_t end = clock();
                float elapsed_time = ((float)(end - start)) / CLOCKS_PER_SEC;
                
                if (valid) {
                    log_validator_message(validator_id, "BLOCK FROM MINER %d VALID!", miner_id);
                    
                    // Adiciona o bloco ao ledger
                    if (add_block_to_ledger(&block, blockchain_ledger, config) == 0) {
                        log_validator_message(validator_id, "BLOCK FROM MINER %d INSERTED IN BLOCKCHAIN!", miner_id);
                        
                        // Remove as transações do pool
                        remove_transactions_from_pool(&block, transaction_pool, config);
                    } else {
                        log_message("VALIDATOR: Failed to add block %s to ledger!", block.txb_id);
                        
                        // Se falhar em adicionar ao ledger, devolver as transações ao pool
                        sem_wait(&transaction_pool->mutex);
                        
                        for (int i = 0; i < block.transactions_per_block; i++) {
                            for (int j = 0; j < config->tx_pool_size; j++) {
                                if (strcmp(transaction_pool->transactions_list[j].transaction.tx_id, block.transactions[i].tx_id) == 0) {
                                    transaction_pool->transactions_list[j].empty = 0;
                                    break;
                                }
                            }
                        }
                        
                        // Recalcula a ocupação
                        int count = 0;
                        for (int i = 0; i < config->tx_pool_size; i++) {
                            if (!transaction_pool->transactions_list[i].empty) {
                                count++;
                            }
                        }
                        
                        transaction_pool->occupancy = (count * 100) / config->tx_pool_size;
                        sem_post(&transaction_pool->mutex);
                    }
                } else {
                    log_validator_message(validator_id, "BLOCK FROM MINER %d INVALID!", miner_id);
                    // A função validate_block já lidou com a devolução das transações
                }
                
                // Envia estatísticas
                send_statistics(msg_queue_id, miner_id, valid, &block, elapsed_time);

            } else {
                 log_message("VALIDATOR: Failed to read complete block data from pipe.");
            }

            // Libera a memória alocada para as transações desta iteração
            if (transactions) {
                free(transactions);
                transactions = NULL; // Avoid double free
            }
        } else if (bytes_read <= 0) {
            // Erro na leitura ou pipe fechado
            if (bytes_read < 0) {
                perror("read miner_id from pipe");
            }
            // Não fazemos nada especial aqui, apenas continuamos o loop
        }
    }
    
    // Fecha o pipe
    close(fd);
    
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
    
    log_message("VALIDATOR %d: Processo encerrado", validator_id);
    
    return EXIT_SUCCESS;
} 