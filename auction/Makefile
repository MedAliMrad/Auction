# ─── Makefile — Système d'Enchères v2.0 ─────────────────────────────────────
CC      = gcc
CFLAGS  = -Wall -Wextra -g -I./include
LDFLAGS = -lpthread

SRC = src
BIN = bin

.PHONY: all clean run-serveur run-client run-bot run-web help

all: $(BIN)/serveur $(BIN)/client $(BIN)/bot

$(BIN):
	mkdir -p $(BIN)

$(BIN)/serveur: $(SRC)/serveur.c include/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  ✓ serveur compile"

$(BIN)/client: $(SRC)/client.c include/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  ✓ client compile"

$(BIN)/bot: $(SRC)/bot.c include/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  ✓ bot compile"

# ─── Lancement rapide ────────────────────────────────────────────────────────

run-serveur: $(BIN)/serveur
	./$(BIN)/serveur $(PORT)

# make run-client IP=10.147.17.5
# make run-client IP=10.147.17.5 PORT=8080
run-client: $(BIN)/client
ifndef IP
	$(error IP non defini. Ex: make run-client IP=10.147.17.5)
endif
	./$(BIN)/client $(IP) $(PORT)

# make run-bot IP=10.147.17.5
# make run-bot IP=10.147.17.5 PORT=8080 MAX=500 ROOM=1
run-bot: $(BIN)/bot
ifndef IP
	$(error IP non defini. Ex: make run-bot IP=127.0.0.1)
endif
	./$(BIN)/bot $(IP) $(or $(PORT),8080) $(or $(MAX),500) $(or $(ROOM),1)

# make run-web IP=127.0.0.1
# make run-web IP=10.147.17.5 PORT=8080 WEBPORT=5000 ROOM=1
run-web:
ifndef IP
	$(error IP non defini. Ex: make run-web IP=127.0.0.1)
endif
	python3 web/server.py $(IP) $(or $(PORT),8080) $(or $(WEBPORT),5000) $(or $(ROOM),1)

# ─── Nettoyage ───────────────────────────────────────────────────────────────
clean:
	rm -rf $(BIN)
	@echo "  ✓ nettoyage"

# ─── Aide ────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  ╔══════════════════════════════════════════════╗"
	@echo "  ║  SYSTEME D'ENCHERES v2.0                     ║"
	@echo "  ╠══════════════════════════════════════════════╣"
	@echo "  ║  Fonctionnalites:                            ║"
	@echo "  ║  • Encheres multiples enchainees             ║"
	@echo "  ║  • Solde par joueur (1000 DT)                ║"
	@echo "  ║  • Mise minimum +10 DT                       ║"
	@echo "  ║  • Timer etendu si mise tardive              ║"
	@echo "  ║  • 4 salles paralleles                       ║"
	@echo "  ║  • Bot automatique                           ║"
	@echo "  ║  • Interface web                             ║"
	@echo "  ║  • Historique + resultats.txt                ║"
	@echo "  ╚══════════════════════════════════════════════╝"
	@echo ""
	@echo "  Compilation:"
	@echo "    make"
	@echo ""
	@echo "  Serveur (PC hote):"
	@echo "    make run-serveur"
	@echo "    make run-serveur PORT=9090"
	@echo ""
	@echo "  Client (chaque joueur):"
	@echo "    make run-client IP=10.147.17.5"
	@echo "    make run-client IP=10.147.17.5 PORT=8080"
	@echo ""
	@echo "  Bot (test automatique):"
	@echo "    make run-bot IP=127.0.0.1"
	@echo "    make run-bot IP=10.147.17.5 MAX=600 ROOM=2"
	@echo ""
	@echo "  Interface Web (navigateur/telephone):"
	@echo "    make run-web IP=127.0.0.1"
	@echo "    make run-web IP=10.147.17.5 WEBPORT=5000 ROOM=1"
	@echo "    → Ouvrir http://localhost:5000 dans le navigateur"
	@echo ""
