#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>

// Nome do arquivo de log
#define LOG_FILENAME "DEIChain_log.log"

// Inicializa o sistema de log
// Retorna 0 em caso de sucesso, -1 em caso de erro
int init_logger();

// Fecha o sistema de log
void close_logger();

// Escreve uma mensagem no log e na tela
void log_message(const char *format, ...);

// Escreve uma mensagem de log para uma thread mineradora específica
void log_miner_message(int miner_id, const char *format, ...);

// Escreve uma mensagem de log para um validador específico
void log_validator_message(int validator_id, const char *format, ...);

// Escreve múltiplas mensagens no log atomicamente (sem interrupção)
void log_atomic_messages(char **messages, int num_messages);

// Retorna o ponteiro para o arquivo de log
FILE *get_log_file();

#endif // LOGGER_H 