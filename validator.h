#ifndef VALIDATOR_H
#define VALIDATOR_H

#include "data_structures.h"

// Função principal do Validator
int validator_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id);

// Função para verificar a validade de um bloco
int validate_block(TransactionBlock *block, BlockchainLedger *ledger, TransactionPool *pool, Config *config);

// Função para adicionar um bloco validado ao ledger
int add_block_to_ledger(TransactionBlock *block, BlockchainLedger *ledger, Config *config);

// Função para gerenciar os validadores dinâmicos baseados na ocupação do pool
void manage_validators(TransactionPool *pool, int *active_validators);

#endif // VALIDATOR_H 