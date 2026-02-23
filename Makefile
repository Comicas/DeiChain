CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
LDFLAGS = -pthread -lssl -lcrypto

DEICHAIN_EXEC = deichain
TXGEN_EXEC    = txgen

DEICHAIN_SRCS = controller.c logger.c miner.c validator.c statistics.c pow.c

TXGEN_SRCS    = txgen.c logger.c

.PHONY: all clean config

# Regra padrão: compila ambos os executáveis
all: $(DEICHAIN_EXEC) $(TXGEN_EXEC)

# Compilação do DEIChain
$(DEICHAIN_EXEC):
	@echo "Compilando $(DEICHAIN_EXEC)..."
	$(CC) $(CFLAGS) $(DEICHAIN_SRCS) -o $(DEICHAIN_EXEC) $(LDFLAGS)

# Compilação do TxGen
$(TXGEN_EXEC):
	@echo "Compilando $(TXGEN_EXEC)..."
	$(CC) $(CFLAGS) $(TXGEN_SRCS) -o $(TXGEN_EXEC) $(LDFLAGS)

clean:
	@echo "Limpando executáveis..."
	rm -f $(DEICHAIN_EXEC) $(TXGEN_EXEC)

# Cria um arquivo de configuração padrão
config:
	@echo "Criando arquivo de configuração padrão (config.cfg)..."
	@echo "5" > config.cfg
	@echo "50" >> config.cfg
	@echo "10" >> config.cfg
	@echo "50000" >> config.cfg
	@echo "Arquivo de configuração criado com valores padrão."
