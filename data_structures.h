#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>

// Defines para depuração
#define DEBUG 1
#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, "%s:%d:%s(): " fmt, \
            __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

// Nome do pipe nomeado para comunicação entre Miner e Validator
#define VALIDATOR_INPUT "/tmp/VALIDATOR_INPUT"

// Definições de hash e blocos
#define TX_ID_LEN 16
#define TXB_ID_LEN 64
#define HASH_SIZE 65  // SHA256_DIGEST_LENGTH * 2 + 1
#define INITIAL_HASH "00006a8e76f31ba74e21a092cca1015a418c9d5f4375e7a4fec676e1d2ec1436"
#define POW_MAX_OPS 10000000

// Estrutura de uma transação
typedef struct {
    char tx_id[TX_ID_LEN];      // ID único da transação (TX-PID-serial)
    int reward;                 // Recompensa (1-3), relacionada à complexidade do PoW
    float value;                // Valor da transação (aleatório)
    time_t timestamp;           // Timestamp da criação
} Transaction;

// Estrutura de um bloco de transações
typedef struct {
    char txb_id[TXB_ID_LEN];              // ID único do bloco (BLOCK-ThreadID-block#)
    char previous_block_hash[HASH_SIZE];  // Hash do bloco anterior
    time_t timestamp;                     // Timestamp da criação
    Transaction *transactions;            // Array de transações
    unsigned int nonce;                   // Nonce encontrado no PoW
    int transactions_per_block;           // Número de transações no bloco
} TransactionBlock;

// Estrutura do resultado do PoW
typedef struct {
    char hash[HASH_SIZE];
    double elapsed_time;
    int operations;
    int error;
} PoWResult;

// Estrutura de entrada no pool de transações
typedef struct {
    Transaction transaction;
    int empty;               // Flag indicando se a posição está disponível (1) ou ocupada (0)
    int age;                 // Idade da transação (incrementada pelo Validator)
} TransactionPoolEntry;

// Estrutura do Pool de Transações (memória compartilhada)
typedef struct {
    int current_block_id;      // ID do bloco atual
    int occupancy;             // Ocupação atual do pool (percentual)
    sem_t mutex;               // Semáforo para acesso ao pool
    sem_t work_notification_sem; // Para notificar miners de novas transações
    TransactionPoolEntry transactions_list[]; // Array de entradas de transações
} TransactionPool;

// Estrutura de um bloco no Blockchain Ledger
typedef struct {
    TransactionBlock block;
    int valid;                // Flag indicando se o bloco é válido
} BlockchainEntry;

// Estrutura do Blockchain Ledger (memória compartilhada)
typedef struct {
    int num_blocks;           // Número atual de blocos na blockchain
    sem_t mutex;              // Semáforo para acesso à blockchain
    BlockchainEntry blocks[]; // Array de blocos
} BlockchainLedger;

// Estrutura da mensagem para comunicação entre Validator e Statistics
typedef struct {
    long mtype;               // Tipo da mensagem
    int miner_id;             // ID do minerador
    int valid;                // Flag de validade do bloco
    float elapsed_time;       // Tempo gasto na validação
    int transaction_count;    // Número de transações
    float total_value;        // Valor total das transações
    int total_rewards;        // Soma das recompensas das transações no bloco
} StatisticsMessage;

// Armazena as configurações lidas do arquivo config.cfg
typedef struct {
    int num_miners;            // Número de mineradores
    int tx_pool_size;          // Tamanho do pool de transações
    int transactions_per_block; // Transações por bloco
    int blockchain_blocks;     // Tamanho máximo da blockchain
} Config;

#endif // DATA_STRUCTURES_H 