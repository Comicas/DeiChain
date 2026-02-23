#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

// Arquivo de log
static FILE *log_file = NULL;

// Mutex para sincronização do log
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Inicializa o sistema de log
int init_logger() {
    // Abre o arquivo de log para escrita (cria um novo se não existir)
    log_file = fopen(LOG_FILENAME, "w");
    if (log_file == NULL) {
        perror("Erro ao abrir arquivo de log");
        return -1;
    }
    
    // Inicializa o mutex
    pthread_mutex_init(&log_mutex, NULL);
    
    return 0;
}

// Fecha o sistema de log
void close_logger() {
    if (log_file != NULL) {
        // Fecha o arquivo
        fclose(log_file);
        log_file = NULL;
        
        // Destrói o mutex
        pthread_mutex_destroy(&log_mutex);
    }
}

// Escreve uma mensagem no log e na tela
void log_message(const char *format, ...) {
    va_list args;
    struct timeval tv;
    struct tm *timeinfo;
    char timestamp[20];
    char full_message[1024];
    
    // Obtém o timestamp atual com maior precisão
    gettimeofday(&tv, NULL);
    timeinfo = localtime(&tv.tv_sec);
    
    // Formata o timestamp como HH:MM:SS
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);
    
    // Formata a mensagem com os argumentos variáveis
    va_start(args, format);
    vsprintf(full_message, format, args);
    va_end(args);
    
    // Adquire o mutex para sincronização
    pthread_mutex_lock(&log_mutex);
    
    // Escreve no console
    printf("%s %s\n", timestamp, full_message);
    
    // Escreve no arquivo de log se estiver aberto
    if (log_file != NULL) {
        fprintf(log_file, "%s %s\n", timestamp, full_message);
        fflush(log_file);  // Força a escrita imediata no arquivo
    }
    
    // Libera o mutex
    pthread_mutex_unlock(&log_mutex);
}

// Escreve múltiplas mensagens no log atomicamente (sem interrupção)
void log_atomic_messages(char **messages, int num_messages) {
    struct timeval tv;
    struct tm *timeinfo;
    char timestamp[20];
    
    // Obtém o timestamp atual com maior precisão
    gettimeofday(&tv, NULL);
    timeinfo = localtime(&tv.tv_sec);
    
    // Formata o timestamp como HH:MM:SS
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);
    
    // Adquire o mutex para sincronização - garante que nenhuma outra thread/processo interrompa
    pthread_mutex_lock(&log_mutex);
    
    // Escreve todas as mensagens de uma vez
    for (int i = 0; i < num_messages; i++) {
        // Escreve no console
        printf("%s %s\n", timestamp, messages[i]);
        
        // Escreve no arquivo de log se estiver aberto
        if (log_file != NULL) {
            fprintf(log_file, "%s %s\n", timestamp, messages[i]);
        }
    }
    
    // Força a escrita imediata no arquivo
    if (log_file != NULL) {
        fflush(log_file);
    }
    
    // Desbloqueia o mutex
    pthread_mutex_unlock(&log_mutex);
}

// Escreve uma mensagem de log para uma thread mineradora específica
void log_miner_message(int miner_id, const char *format, ...) {
    va_list args;
    struct timeval tv;
    struct tm *timeinfo;
    char timestamp[20];
    char message[1024];
    char full_message[1100];
    
    // Obtém o timestamp atual
    gettimeofday(&tv, NULL);
    timeinfo = localtime(&tv.tv_sec);
    
    // Formata o timestamp como HH:MM:SS
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);
    
    // Formata a mensagem com os argumentos variáveis
    va_start(args, format);
    vsprintf(message, format, args);
    va_end(args);
    
    // Cria a mensagem completa com o prefixo do minerador
    snprintf(full_message, sizeof(full_message), "MINER %d: %s", miner_id, message);
    
    // Adquire o mutex para sincronização
    pthread_mutex_lock(&log_mutex);
    
    // Escreve no console
    printf("%s %s\n", timestamp, full_message);
    
    // Escreve no arquivo de log se estiver aberto
    if (log_file != NULL) {
        fprintf(log_file, "%s %s\n", timestamp, full_message);
        fflush(log_file);
    }
    
    // Libera o mutex
    pthread_mutex_unlock(&log_mutex);
}

// Escreve uma mensagem de log para um validador específico
void log_validator_message(int validator_id, const char *format, ...) {
    va_list args;
    struct timeval tv;
    struct tm *timeinfo;
    char timestamp[20];
    char message[1024];
    char full_message[1100];
    
    // Obtém o timestamp atual
    gettimeofday(&tv, NULL);
    timeinfo = localtime(&tv.tv_sec);
    
    // Formata o timestamp como HH:MM:SS
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);
    
    // Formata a mensagem com os argumentos variáveis
    va_start(args, format);
    vsprintf(message, format, args);
    va_end(args);
    
    // Cria a mensagem completa com o prefixo do validador
    snprintf(full_message, sizeof(full_message), "VALIDATOR %d: %s", validator_id, message);
    
    // Adquire o mutex para sincronização
    pthread_mutex_lock(&log_mutex);
    
    // Escreve no console
    printf("%s %s\n", timestamp, full_message);
    
    // Escreve no arquivo de log se estiver aberto
    if (log_file != NULL) {
        fprintf(log_file, "%s %s\n", timestamp, full_message);
        fflush(log_file);
    }
    
    // Libera o mutex
    pthread_mutex_unlock(&log_mutex);
}

// Retorna o ponteiro para o arquivo de log
FILE *get_log_file() {
    return log_file;
} 