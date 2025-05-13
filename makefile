# Makefile mis à jour
CC = gcc
CFLAGS = -Wall -Wextra -pthread
LDFLAGS = -pthread
OBJDIR = bin
BINDIR = bin

# Object files in bin/
OBJS_CLIENT = $(OBJDIR)/client.o $(OBJDIR)/common.o
OBJS_SERVER = $(OBJDIR)/server.o $(OBJDIR)/common.o $(OBJDIR)/command.o

all: $(BINDIR)/client $(BINDIR)/server

# Ensure bin/ exists before compiling
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Compile object files into bin/
$(OBJDIR)/common.o: common.c common.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c common.c -o $@

$(OBJDIR)/client.o: client.c client.h common.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c client.c -o $@

$(OBJDIR)/server.o: server.c server.h common.h command.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c server.c -o $@

$(OBJDIR)/command.o: command.c command.h common.h server.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c command.c -o $@

# Link executables into bin/
$(BINDIR)/client: $(OBJS_CLIENT)
	$(CC) $(LDFLAGS) $^ -o $@

$(BINDIR)/server: $(OBJS_SERVER)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -f $(OBJDIR)/*.o $(BINDIR)/client $(BINDIR)/server

.PHONY: all clean