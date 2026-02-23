#ifndef POW_H
#define POW_H

#include "data_structures.h"

// Definition of Difficulty Levels
typedef enum { EASY = 1, NORMAL = 2, HARD = 3 } DifficultyLevel;

// Function prototypes for PoW
void compute_sha256(const TransactionBlock *input, char *output);
PoWResult proof_of_work(TransactionBlock *block);
int verify_nonce(const TransactionBlock *block);
int check_difficulty(const char *hash, const int reward);
DifficultyLevel getDifficultFromReward(const int reward);

#endif // POW_H 