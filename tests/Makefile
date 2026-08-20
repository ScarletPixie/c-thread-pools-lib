CC = cc
CFLAGS = -Wall -Wextra -Werror -g3

SRC = ../ctp.c
TEST_SRC = main.c

TEST_BIN = test_ctp

all: $(TEST_BIN)

$(TEST_BIN): $(SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $(TEST_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(TEST_BIN)

re: clean all

.PHONY: all test clean re