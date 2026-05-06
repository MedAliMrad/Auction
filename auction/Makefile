# ─── Makefile — Système d'Enchères ──────────────────────────────────────────
CC      = gcc
CFLAGS  = -Wall -Wextra -g -I./include
LDFLAGS = -lpthread

SRC = src
BIN = bin

.PHONY: all clean run-serveur run-client help

all: $(BIN)/serveur $(BIN)/client

$(BIN):
	mkdir -p $(BIN)

$(BIN)/serveur: $(SRC)/serveur.c include/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  ok  serveur compile"

$(BIN)/client: $(SRC)/client.c include/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  ok  client compile"

# ─── Lancement rapide ────────────────────────────────────────────────────────

# Terminal 1 : lancer le serveur
run-serveur: $(BIN)/serveur
	./$(BIN)/serveur $(PORT)

# Terminal 2+ : lancer un client
# make run-client IP=192.168.1.10
# make run-client IP=0.tcp.ngrok.io PORT=12345
run-client: $(BIN)/client
ifndef IP
	$(error IP non defini. Ex: make run-client IP=192.168.1.10)
endif
	./$(BIN)/client $(IP) $(PORT)

# ─── Nettoyage ───────────────────────────────────────────────────────────────
clean:
	rm -rf $(BIN)
	@echo "  ok  nettoyage"

# ─── Aide ────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  SYSTEME D'ENCHERES EN TEMPS REEL"
	@echo "  ================================="
	@echo ""
	@echo "  Compilation :"
	@echo "    make"
	@echo ""
	@echo "  Sur le PC serveur (Ubuntu / VirtualBox) :"
	@echo "    make run-serveur"
	@echo "    make run-serveur PORT=9090    (port custom)"
	@echo ""
	@echo "  Sur chaque PC / telephone client :"
	@echo "    make run-client IP=192.168.1.10"
	@echo "    make run-client IP=192.168.1.10 PORT=8080"
	@echo ""
	@echo "  Avec Ngrok (depuis internet) :"
	@echo "    ngrok tcp 8080                            (sur le serveur)"
	@echo "    make run-client IP=0.tcp.ngrok.io PORT=XXXXX"
	@echo ""
	@echo "  Depuis Termux (telephone Android) :"
	@echo "    pkg install clang make"
	@echo "    make"
	@echo "    ./bin/client 0.tcp.ngrok.io XXXXX"
	@echo ""