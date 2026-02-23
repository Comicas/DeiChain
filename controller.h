#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "data_structures.h"

// Função para ler o arquivo de configuração
Config read_config(const char *filename);

// Função para inicializar recursos do IPC
void init_ipc_resources(Config config, int *transaction_pool_id, int *blockchain_ledger_id, 
                        int *msg_queue_id, int *config_shm_id,
                        TransactionPool **transaction_pool, 
                        BlockchainLedger **blockchain_ledger,
                        Config **shared_config);

// Função para criar os processos filhos
void create_processes(Config config, int transaction_pool_id, int blockchain_ledger_id, 
                     int msg_queue_id, int config_shm_id, pid_t *miner_pid, pid_t *validator_pid, pid_t *statistics_pid);

// Função para limpar recursos quando o sistema é encerrado
void cleanup_resources(int transaction_pool_id, int blockchain_ledger_id, 
                      int msg_queue_id, int config_shm_id, pid_t miner_pid, pid_t validator_pid, pid_t statistics_pid);

// Função de manipulação de sinais
void signal_handler(int sig);

// Função principal do Controller
int controller_main(int argc, char *argv[]);

#endif // CONTROLLER_H 