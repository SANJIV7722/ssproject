CC ?= gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -O2 -D_GNU_SOURCE -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1

# Detect Windows environment for ws2_32 link
ifeq ($(OS),Windows_NT)
    LIBS = -lws2_32
    SERVER_BIN = server.exe
    CLIENT_BIN = client.exe
    RM = del /Q /F
else
    LIBS = -pthread
    SERVER_BIN = server
    CLIENT_BIN = client
    RM = rm -rf
endif

COMMON_SRCS = models.c
SERVER_SRCS = server.c storage.c approval.c $(COMMON_SRCS)
CLIENT_SRCS = client.c $(COMMON_SRCS)

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRCS) models.h protocol.h storage.h approval.h
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRCS) $(LIBS)

$(CLIENT_BIN): $(CLIENT_SRCS) models.h protocol.h
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRCS) $(LIBS)

clean:
	$(RM) $(SERVER_BIN) $(CLIENT_BIN) *.o
