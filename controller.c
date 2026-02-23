#include "controller.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <semaphore.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// Variáveis globais para manipulação de sinal
static int transaction_pool_id_g = -1;
static int blockchain_ledger_id_g = -1;
static int msg_queue_id_g = -1;
static int config_shm_id_g = -1;
static pid_t miner_pid_g = -1;
static pid_t validator_pid_g = -1;
static pid_t statistics_pid_g = -1;
static volatile sig_atomic_t should_terminate = 0;
static volatile sig_atomic_t sigusr1_received_controller = 0;

// Função para ler o arquivo de configuração
Config read_config(const char *filename) {
    Config config = {0};
    FILE *file = fopen(filename, "r");
    
    if (file == NULL) {
        log_message("CONTROLLER: Erro ao abrir arquivo de configuração: %s", filename);
        exit(EXIT_FAILURE);
    }
    
    // Lê as configurações
    fscanf(file, "%d", &config.num_miners);
    fscanf(file, "%d", &config.tx_pool_size);
    fscanf(file, "%d", &config.transactions_per_block);
    fscanf(file, "%d", &config.blockchain_blocks);
    
    fclose(file);
    
    // Valida as configurações
    if (config.num_miners <= 0 || config.tx_pool_size <= 0 || 
        config.transactions_per_block <= 0 || config.blockchain_blocks <= 0) {
        log_message("CONTROLLER: Configuração inválida no arquivo: %s", filename);
        exit(EXIT_FAILURE);
    }
    
    log_message("CONTROLLER: Configuração carregada: NUM_MINERS=%d, TX_POOL_SIZE=%d, TRANSACTIONS_PER_BLOCK=%d, BLOCKCHAIN_BLOCKS=%d",
                config.num_miners, config.tx_pool_size, config.transactions_per_block, 
                config.blockchain_blocks);
    
    return config;
}

// Função para inicializar recursos do IPC
void init_ipc_resources(Config config, int *transaction_pool_id, int *blockchain_ledger_id, 
                        int *msg_queue_id, int *config_shm_id,
                        TransactionPool **transaction_pool, 
                        BlockchainLedger **blockchain_ledger,
                        Config **shared_config) {
    key_t key;
    
    // Inicializa a memória compartilhada para a Config
    key = ftok(".", 'C');
    *config_shm_id = shmget(key, sizeof(Config), IPC_CREAT | 0666);
    if (*config_shm_id == -1) {
        perror("shmget Config");
        exit(EXIT_FAILURE);
    }
    
    // Anexa a memória compartilhada para a Config
    *shared_config = (Config*)shmat(*config_shm_id, NULL, 0);
    if (*shared_config == (void*)-1) {
        perror("shmat Config");
        exit(EXIT_FAILURE);
    }
    
    // Copia a configuração para a memória compartilhada
    memcpy(*shared_config, &config, sizeof(Config));
    
    log_message("CONTROLLER: Memória compartilhada da Config criada (ID: %d)", *config_shm_id);
    
    // Inicializa a memória compartilhada para o Pool de Transações
    key = ftok(".", 'T');
    *transaction_pool_id = shmget(key, sizeof(TransactionPool) + config.tx_pool_size * sizeof(TransactionPoolEntry), 
                                IPC_CREAT | 0666);
    if (*transaction_pool_id == -1) {
        perror("shmget TransactionPool");
        exit(EXIT_FAILURE);
    }
    
    // Anexa ao espaço de endereçamento do processo
    *transaction_pool = (TransactionPool*)shmat(*transaction_pool_id, NULL, 0);
    if (*transaction_pool == (void*)-1) {
        perror("shmat TransactionPool");
        exit(EXIT_FAILURE);
    }
    
    // Inicializa o Pool de Transações
    (*transaction_pool)->current_block_id = 0;
    (*transaction_pool)->occupancy = 0;
    
    // Inicializa o semáforo do pool
    if (sem_init(&((*transaction_pool)->mutex), 1, 1) == -1) {
        perror("sem_init TransactionPool mutex");
        exit(EXIT_FAILURE);
    }
    
    // Inicializa o NOVO semáforo de notificação de trabalho
    if (sem_init(&((*transaction_pool)->work_notification_sem), 1, 0) == -1) {
        perror("sem_init TransactionPool work_notification_sem");
        exit(EXIT_FAILURE);
    }
    
    // Inicializa as entradas do pool como vazias
    for (int i = 0; i < config.tx_pool_size; i++) {
        (*transaction_pool)->transactions_list[i].empty = 1;
        (*transaction_pool)->transactions_list[i].age = 0;
    }
    
    log_message("CONTROLLER: SHM_TX_POOL CREATED");
    
    // Inicializa a memória compartilhada para o Blockchain Ledger
    key = ftok(".", 'B');
    // Calculate the total size needed: Ledger struct + array of entries + space for all transactions
    size_t entry_struct_size = sizeof(BlockchainEntry); // Size of one entry structure
    size_t block_transactions_size = config.transactions_per_block * sizeof(Transaction); // Size of transactions for one block
    size_t total_ledger_shm_size = sizeof(BlockchainLedger) + config.blockchain_blocks * (entry_struct_size + block_transactions_size);

    *blockchain_ledger_id = shmget(key, total_ledger_shm_size, IPC_CREAT | 0666);
    if (*blockchain_ledger_id == -1) {
        perror("shmget BlockchainLedger");
        exit(EXIT_FAILURE);
    }
    
    // Anexa ao espaço de endereçamento do processo
    *blockchain_ledger = (BlockchainLedger*)shmat(*blockchain_ledger_id, NULL, 0);
    if (*blockchain_ledger == (void*)-1) {
        perror("shmat BlockchainLedger");
        exit(EXIT_FAILURE);
    }
    
    // Inicializa o Blockchain Ledger
    (*blockchain_ledger)->num_blocks = 0;
    
    // Inicializa o semáforo do ledger
    if (sem_init(&((*blockchain_ledger)->mutex), 1, 1) == -1) {
        perror("sem_init BlockchainLedger");
        exit(EXIT_FAILURE);
    }
    
    log_message("CONTROLLER: SHM_LEDGER CREATED");
    
    // Cria a fila de mensagens para comunicação entre Validator e Statistics
    key = ftok(".", 'M');
    *msg_queue_id = msgget(key, IPC_CREAT | 0666);
    if (*msg_queue_id == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }
    
    log_message("CONTROLLER: MESSAGE QUEUE CREATED");
    
    // Cria o named pipe para comunicação entre Miner e Validator
    if (mkfifo(VALIDATOR_INPUT, 0666) == -1) {
        // Se o pipe já existe, não é um erro crítico
        if (errno != EEXIST) {
            perror("mkfifo");
            exit(EXIT_FAILURE);
        }
    }
    
    log_message("CONTROLLER: NAMED PIPE CREATED");
}

// Declarações externas para os pontos de entrada dos outros processos
extern int miner_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id);
extern int validator_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id);
extern int statistics_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id);

// Função para criar os processos filhos
void create_processes(Config config __attribute__((unused)), int transaction_pool_id, int blockchain_ledger_id, 
                     int msg_queue_id, int config_shm_id, pid_t *miner_pid, pid_t *validator_pid, pid_t *statistics_pid) {
    // Cria o processo Miner
    *miner_pid = fork();
    if (*miner_pid == -1) {
        perror("fork Miner");
        exit(EXIT_FAILURE);
    } else if (*miner_pid == 0) {
        // Código do processo filho Miner
        exit(miner_main(transaction_pool_id, blockchain_ledger_id, msg_queue_id, config_shm_id));
    }
    
    log_message("CONTROLLER: PROCESS MINER CREATED");
    
    // Cria o processo Validator
    *validator_pid = fork();
    if (*validator_pid == -1) {
        perror("fork Validator");
        exit(EXIT_FAILURE);
    } else if (*validator_pid == 0) {
        // Código do processo filho Validator
        exit(validator_main(transaction_pool_id, blockchain_ledger_id, msg_queue_id, config_shm_id));
    }
    
    log_message("CONTROLLER: PROCESS VALIDATOR CREATED");
    
    // Cria o processo Statistics
    *statistics_pid = fork();
    if (*statistics_pid == -1) {
        perror("fork Statistics");
        exit(EXIT_FAILURE);
    } else if (*statistics_pid == 0) {
        // Código do processo filho Statistics
        exit(statistics_main(transaction_pool_id, blockchain_ledger_id, msg_queue_id, config_shm_id));
    }
    
    log_message("CONTROLLER: PROCESS STATISTICS CREATED");
}

// Função para limpar recursos quando o sistema é encerrado
void cleanup_resources(int transaction_pool_id, int blockchain_ledger_id, 
                      int msg_queue_id, int config_shm_id, pid_t miner_pid, pid_t validator_pid, pid_t statistics_pid) {
    // Envia sinal de terminação para os processos filhos
    if (miner_pid > 0) {
        kill(miner_pid, SIGTERM);
        waitpid(miner_pid, NULL, 0);
        log_message("CONTROLLER: Processo Miner terminado (PID: %d)", miner_pid);
    }
    
    if (validator_pid > 0) {
        kill(validator_pid, SIGTERM);
        waitpid(validator_pid, NULL, 0);
        log_message("CONTROLLER: Processo Validator terminado (PID: %d)", validator_pid);
    }
    
    if (statistics_pid > 0) {
        kill(statistics_pid, SIGTERM);
        waitpid(statistics_pid, NULL, 0);
        log_message("CONTROLLER: Processo Statistics terminado (PID: %d)", statistics_pid);
    }
    
    // Remove os recursos de IPC
    if (transaction_pool_id != -1) {
        // Obtém acesso à memória compartilhada para destruir o semáforo
        TransactionPool *pool = (TransactionPool*)shmat(transaction_pool_id, NULL, 0);
        if (pool != (void*)-1) {
            sem_destroy(&(pool->mutex));
            sem_destroy(&(pool->work_notification_sem));
            shmdt(pool);
        }
        shmctl(transaction_pool_id, IPC_RMID, NULL);
        log_message("CONTROLLER: Memória compartilhada do Pool de Transações removida");
    }
    
    if (blockchain_ledger_id != -1) {
        // Obtém acesso à memória compartilhada para destruir o semáforo
        BlockchainLedger *ledger = (BlockchainLedger*)shmat(blockchain_ledger_id, NULL, 0);
        if (ledger != (void*)-1) {
            sem_destroy(&(ledger->mutex));
            shmdt(ledger);
        }
        shmctl(blockchain_ledger_id, IPC_RMID, NULL);
        log_message("CONTROLLER: Memória compartilhada do Blockchain Ledger removida");
    }
    
    if (config_shm_id != -1) {
        shmctl(config_shm_id, IPC_RMID, NULL);
        log_message("CONTROLLER: Memória compartilhada da Config removida");
    }
    
    if (msg_queue_id != -1) {
        msgctl(msg_queue_id, IPC_RMID, NULL);
        log_message("CONTROLLER: Fila de mensagens removida");
    }
    
    // Remove o named pipe
    unlink(VALIDATOR_INPUT);
    log_message("CONTROLLER: Named pipe removido");
}

// Função para imprimir o conteúdo do Blockchain Ledger
void dump_ledger(BlockchainLedger *ledger, Config *config) {
    #define MAX_LEDGER_LINES 1000 // Máximo de linhas para o ledger
    char **messages = (char**)malloc(MAX_LEDGER_LINES * sizeof(char*));
    int msg_idx = 0;
    
    // Aloca memória para cada linha da mensagem
    for (int i = 0; i < MAX_LEDGER_LINES; i++) {
        messages[i] = (char*)malloc(1024 * sizeof(char));
        if (!messages[i]) {
            perror("malloc messages");
            for (int j = 0; j < i; j++) {
                free(messages[j]);
            }
            free(messages);
            return;
        }
    }
    
    strcpy(messages[msg_idx++], "CONTROLLER: Dumping the Ledger (SIGUSR1 Received)");
    strcpy(messages[msg_idx++], "");
    strcpy(messages[msg_idx++], "=================== Start Ledger ===================");
    
    // Percorre todos os blocos válidos
    for (int i = 0; i < ledger->num_blocks; i++) {
        if (ledger->blocks[i].valid) {
            TransactionBlock *block = &(ledger->blocks[i].block);
            
            snprintf(messages[msg_idx++], 1024, "||---- Block %03d --", i);
            snprintf(messages[msg_idx++], 1024, "Block ID: %s", block->txb_id);
            snprintf(messages[msg_idx++], 1024, "Previous Hash:");
            snprintf(messages[msg_idx++], 1024, "%s", block->previous_block_hash);
            snprintf(messages[msg_idx++], 1024, "Block Timestamp: %ld", block->timestamp);
            snprintf(messages[msg_idx++], 1024, "Nonce: %u", block->nonce);
            snprintf(messages[msg_idx++], 1024, "Transactions:");
            
            void *entries_base = (void *)ledger->blocks;
            void *transactions_base = entries_base + (config->blockchain_blocks * sizeof(BlockchainEntry));
            size_t block_tx_offset = i * config->transactions_per_block * sizeof(Transaction);
            Transaction *transactions_ptr = (Transaction *)(transactions_base + block_tx_offset);
            
            // Imprime as transações do bloco using the recalculated pointer
            for (int j = 0; j < config->transactions_per_block; j++) {
                // Aceder às transações usando o ponteiro calculado 'transactions_ptr'
                snprintf(messages[msg_idx++], 1024, " [%d] ID: %s | Reward: %d | Value: %.2f | Timestamp: %ld",
                       j, transactions_ptr[j].tx_id, transactions_ptr[j].reward,
                       transactions_ptr[j].value, transactions_ptr[j].timestamp);
            }
            
            snprintf(messages[msg_idx++], 1024, "||------------------------------");
        }
    }
    
    strcpy(messages[msg_idx++], "=================== End Ledger ===================");
    strcpy(messages[msg_idx++], "");
    
    // Enviar todas as mensagens atomicamente
    log_atomic_messages(messages, msg_idx);
    
    // Libera a memória alocada
    for (int i = 0; i < MAX_LEDGER_LINES; i++) {
        free(messages[i]);
    }
    free(messages);
}

// Função de manipulação de sinais
void signal_handler(int sig) {
    if (sig == SIGINT) {
        log_message("CONTROLLER: SIGNAL SIGINT RECEIVED");
        should_terminate = 1;
    } else if (sig == SIGUSR1) {
        sigusr1_received_controller = 1;
    }
}

// Função principal do Controller
int controller_main(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    // Inicializa o logger
    if (init_logger() != 0) {
        fprintf(stderr, "Falha ao inicializar o logger\n");
        exit(EXIT_FAILURE);
    }
    
    log_message("CONTROLLER: DEI_CHAIN SIMULATOR STARTING");
    
    // Configura o manipulador de sinais para SIGINT
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(EXIT_FAILURE);
    }
    // Configura o manipulador de sinais para SIGUSR1
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction SIGUSR1");
        exit(EXIT_FAILURE);
    }
    
    // Lê o arquivo de configuração
    Config config = read_config("config.cfg");
    
    // Inicializa recursos de IPC
    TransactionPool *transaction_pool;
    BlockchainLedger *blockchain_ledger;
    Config *shared_config;
    init_ipc_resources(config, &transaction_pool_id_g, &blockchain_ledger_id_g, &msg_queue_id_g,
                      &config_shm_id_g, &transaction_pool, &blockchain_ledger, &shared_config);
    
    // Cria os processos filhos
    create_processes(config, transaction_pool_id_g, blockchain_ledger_id_g, msg_queue_id_g,
                    config_shm_id_g, &miner_pid_g, &validator_pid_g, &statistics_pid_g);

    // Log PIDs para teste
    log_message("CONTROLLER: Controller PID = %d", getpid());
    log_message("CONTROLLER: Miner Process PID = %d", miner_pid_g);
    log_message("CONTROLLER: Validator Process PID = %d", validator_pid_g);
    log_message("CONTROLLER: Statistics Process PID = %d", statistics_pid_g);

    // Aguarda o sinal de término
    while (!should_terminate) {
        if (sigusr1_received_controller) {
            dump_ledger(blockchain_ledger, shared_config);
            sigusr1_received_controller = 0;
        }
        pause();
    }
    
    log_message("CONTROLLER: WAITING FOR LAST TASKS TO FINISH");
    
    // Limpa recursos e termina processos.
    cleanup_resources(transaction_pool_id_g, blockchain_ledger_id_g, msg_queue_id_g,
                     config_shm_id_g, miner_pid_g, validator_pid_g, statistics_pid_g);

    // Dumps do ledger APÓS as estatísticas terem sido impressas e os processos terminados.
    log_message("CONTROLLER: Performing final Ledger dump");
    dump_ledger(blockchain_ledger, shared_config);
    
    log_message("CONTROLLER: CLOSING SIMULATION");
    
    // Encerra o logger
    close_logger();
    
    return EXIT_SUCCESS;
}

// Função principal do sistema
int main(int argc, char *argv[]) {
    // Inicializa a biblioteca OpenSSL para multi-threading
    if (OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                           OPENSSL_INIT_LOAD_SSL_STRINGS |
                           OPENSSL_INIT_LOAD_CONFIG, NULL) == 0) {
        fprintf(stderr, "OpenSSL initialization failed:\n");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // Inicia o Controller, que gerencia todo o sistema
    return controller_main(argc, argv);
} 