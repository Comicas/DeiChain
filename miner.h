#ifndef MINER_H
#define MINER_H

#include "data_structures.h"

// Estrutura de argumentos para as threads miner
typedef struct {
    int id;                         // ID da thread
    TransactionPool *transaction_pool;   // Referência ao pool de transações
    BlockchainLedger *blockchain_ledger; // Referência ao blockchain ledger
    Config *config;                 // Referência à configuração
    int *should_terminate;          // Flag para término
} MinerThreadArgs;

// Função principal do Miner
int miner_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id);

// Função executada por cada thread Miner
void *miner_thread_function(void *arg);

// Função para selecionar transações do pool para um novo bloco
Transaction *select_transactions(TransactionPool *pool, int transactions_per_block, int miner_id, Config *config);

// Função para criar um novo bloco
TransactionBlock *create_block(Transaction *transactions, int transactions_per_block, char *previous_hash, int miner_id, int block_id);

#endif // MINER_H 