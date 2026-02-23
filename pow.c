#include "pow.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/sha.h>
#include <string.h> 

// Função para calcular uma soma de verificação simples para depuração
unsigned long simple_checksum(const unsigned char *data, size_t len) {
    unsigned long checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        checksum += data[i];
    }
    return checksum;
}

// Bloco de serialização revisto
unsigned char *serialize_block(const TransactionBlock *block, size_t *buffer_size) {
    size_t transactions_data_size = 0;
    // Calcular o tamanho dos dados de transação de forma segura
    if (block->transactions_per_block > 0 && block->transactions != NULL) {
         transactions_data_size = sizeof(Transaction) * block->transactions_per_block;
    } else if (block->transactions_per_block > 0) {
        transactions_data_size = sizeof(Transaction) * block->transactions_per_block;
    }


    // Tamanho total: Todos os campos exceto o ponteiro + dados reais das transações
     *buffer_size = sizeof(TransactionBlock) - sizeof(Transaction*) + transactions_data_size;

    // Utilizar calloc para garantir que o buffer inteiro é inicializado a zero
    unsigned char *buffer = calloc(1, *buffer_size);
    if (!buffer) {
        perror("serialize_block calloc");
        return NULL;
    }
    unsigned char *ptr = buffer;

    // Copiar campos antes do ponteiro de transações na definição da struct
    // 1. txb_id
    memcpy(ptr, block->txb_id, TXB_ID_LEN);
    #ifdef DEBUG_SERIALIZATION_DETAIL
    printf("  TXB_ID Checksum: %lu\n", simple_checksum((unsigned char*)block->txb_id, TXB_ID_LEN));
    #endif
    ptr += TXB_ID_LEN;
    // 2. previous_block_hash
    memcpy(ptr, block->previous_block_hash, HASH_SIZE);
    #ifdef DEBUG_SERIALIZATION_DETAIL
    printf("  PREV_HASH Checksum: %lu\n", simple_checksum((unsigned char*)block->previous_block_hash, HASH_SIZE));
    #endif
    ptr += HASH_SIZE;
    // 3. timestamp
    memcpy(ptr, &block->timestamp, sizeof(time_t));
    #ifdef DEBUG_SERIALIZATION_DETAIL
    printf("  TIMESTAMP Checksum: %lu\n", simple_checksum((unsigned char*)&block->timestamp, sizeof(time_t)));
    #endif
    ptr += sizeof(time_t);

    // Copiar campos depois do ponteiro de transações na definição da struct
    // 4. nonce
    memcpy(ptr, &block->nonce, sizeof(unsigned int));
    #ifdef DEBUG_SERIALIZATION_DETAIL
    printf("  NONCE Val: %u\n", block->nonce);
    #endif
    ptr += sizeof(unsigned int);
    // 5. transactions_per_block
    memcpy(ptr, &block->transactions_per_block, sizeof(int));
    #ifdef DEBUG_SERIALIZATION_DETAIL
    printf("  TX_PER_BLOCK Val: %d\n", block->transactions_per_block);
    #endif
    ptr += sizeof(int);

    // Anexar os dados reais das transações
    if (transactions_data_size > 0 && block->transactions != NULL) {
        memcpy(ptr, block->transactions, transactions_data_size);
        #ifdef DEBUG_SERIALIZATION_DETAIL
        printf("  TRANSACTIONS Checksum: %lu\n", simple_checksum((unsigned char*)block->transactions, transactions_data_size));
        #endif
        // ptr += transactions_data_size; // ptr está agora no final
    } else if (transactions_data_size > 0) {
        // block->transactions era NULL ou a contagem era inconsistente, preencher com zeros o espaço de dados das transações
        memset(ptr, 0, transactions_data_size);
        // ptr += transactions_data_size;
    }

    return buffer;
}

// Calcula o hash SHA-256 de um bloco de transações
void compute_sha256(const TransactionBlock *block, char hash_output[HASH_SIZE]) {
    // Serializa o bloco para um buffer
    size_t buffer_size;
    unsigned char *serialized_block = serialize_block(block, &buffer_size);
    if (!serialized_block) {
        snprintf(hash_output, HASH_SIZE, "ERROR");
        return;
    }

    // DEBUG: Imprime o tamanho do buffer e a soma de verificação
    #ifdef DEBUG_SERIALIZATION
    unsigned long chksum = simple_checksum(serialized_block, buffer_size);
    printf("DEBUG_SERIALIZATION: Block %s - Buffer Size: %zu, Checksum: %lu\n", block->txb_id, buffer_size, chksum);
    #endif

    // 2. Calcular o hash SHA-256 usando OpenSSL
    unsigned char hash[SHA256_DIGEST_LENGTH]; // buffer binário do hash (na stack)
    
    SHA256(serialized_block, buffer_size, hash);

    #ifdef DEBUG_SERIALIZATION_DETAIL
    printf("  BINARY HASH: ");
    for(int k=0; k<SHA256_DIGEST_LENGTH; k++) { printf("%02x", hash[k]); }
    printf("\n");
    #endif

    // Converte para formato hexadecimal para armazenar como string
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash_output + (i * 2), "%02x", hash[i]);
    }
    hash_output[HASH_SIZE - 1] = '\0'; // Garante que termina com null

    // Libera a memória do buffer serializado
    free(serialized_block);
}

// Verifica se um hash atende ao critério de dificuldade (n zeros iniciais)
int check_difficulty(const char *hash, const int reward) {
    // Obtém o nível de dificuldade com base na recompensa
    DifficultyLevel difficulty = getDifficultFromReward(reward);
    int zeros_required;

    switch (difficulty) {
        case EASY:
            zeros_required = 2;
            break;
        case NORMAL:
            zeros_required = 3;
            break;
        case HARD:
            zeros_required = 4;
            break;
        default:
            zeros_required = 2; 
            break;
    }

    // Garante que zeros_required seja pelo menos 1 e não exceda um limite razoável
    // (por exemplo, HASH_SIZE - 1, embora na prática seja muito menor)
    if (zeros_required < 1) {
        zeros_required = 1; // Mínimo de 1 zero
    }
    if (zeros_required > (HASH_SIZE -1) ) {
         zeros_required = HASH_SIZE -1;
    }


    // Verifica o número necessário de zeros iniciais
    for (int i = 0; i < zeros_required; i++) {
        if (hash[i] != '0') {
            return 0; // Não atende ao critério
        }
    }
    return 1; // Atende ao critério
}

// Obtém o nível de dificuldade com base na recompensa
DifficultyLevel getDifficultFromReward(const int reward) {
    switch (reward) {
        case 1:
            return EASY;
        case 2:
            return NORMAL;
        default:
            if (reward >= 3) {
                return HARD;
            } else {
                return EASY; 
            }
    }
}

// Verifica se o nonce é válido para um bloco
int verify_nonce(const TransactionBlock *block) {
    char hash[HASH_SIZE];
    compute_sha256(block, hash);

    // Encontra a maior recompensa para determinar a dificuldade
    int max_reward = 0;
    if (block->transactions != NULL) {
        for (int i = 0; i < block->transactions_per_block; i++) {
            if (block->transactions[i].reward > max_reward) {
                max_reward = block->transactions[i].reward;
            }
        }
    } else if (block->transactions_per_block > 0) {
        max_reward = 1; // Default to EASY
    }

    // Verifica se o hash atende ao critério de dificuldade
    return check_difficulty(hash, max_reward);
}

// Realiza o Proof of Work para encontrar um nonce válido
PoWResult proof_of_work(TransactionBlock *block) {
    PoWResult result;
    char hash[HASH_SIZE];
    clock_t start, end;
    int operations = 0;
    int max_reward = 0;

    // Inicializa o resultado
    result.error = 0;
    result.operations = 0;
    result.elapsed_time = 0.0;
    strcpy(result.hash, "");

    // Encontra a maior recompensa entre as transações para definir a dificuldade
    if (block->transactions != NULL) {
        for (int i = 0; i < block->transactions_per_block; i++) {
            if (block->transactions[i].reward > max_reward) {
                max_reward = block->transactions[i].reward;
            }
        }
    } else if (block->transactions_per_block > 0) {
        max_reward = 1;
    }

    // Inicia a contagem de tempo
    start = clock();

    // Inicializa o nonce
    block->nonce = 0;

    // Loop para encontrar um nonce válido
    while (block->nonce < POW_MAX_OPS) {
        // Incrementa o contador de operações
        operations++;

        // Calcula o hash do bloco
        compute_sha256(block, hash);

        // Verifica se o hash atende ao critério de dificuldade
        if (check_difficulty(hash, max_reward)) {
            // Nonce válido encontrado
            end = clock();
            result.operations = operations;
            result.elapsed_time = ((double) (end - start)) / CLOCKS_PER_SEC;
            strcpy(result.hash, hash);
            return result;
        }

        // Incrementa o nonce para a próxima tentativa
        block->nonce++;

        // Verifica se atingiu o número máximo de operações
        if (operations >= POW_MAX_OPS) {
            break;
        }
    }

    // Não foi possível encontrar um nonce válido
    end = clock();
    result.error = 1;
    result.operations = operations;
    result.elapsed_time = ((double) (end - start)) / CLOCKS_PER_SEC;

    return result;
} 