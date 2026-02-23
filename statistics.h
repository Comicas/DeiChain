#ifndef STATISTICS_H
#define STATISTICS_H

#include "data_structures.h"

// Função principal do Statistics
int statistics_main(int transaction_pool_id, int blockchain_ledger_id, int msg_queue_id, int config_shm_id);

// Função para exibir estatísticas
void print_statistics(int *valid_blocks, int *invalid_blocks, float *avg_validation_time, int *credits, int num_miners);

#endif // STATISTICS_H 