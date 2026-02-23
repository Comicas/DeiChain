#ifndef TXGEN_H
#define TXGEN_H

#include "data_structures.h"

// Função principal do Transaction Generator
int txgen_main(int transaction_pool_id, int config_shm_id, int reward, int sleep_time);

// Função para gerar uma transação aleatória
Transaction generate_transaction(int reward);

#endif // TXGEN_H 