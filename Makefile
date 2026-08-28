CC := cc
CFLAGS := -Wall -Wextra -Werror -MMD -MP -O2

NAME := libctp

BUILD ?= ./build
BUILD := $(patsubst %/,%,$(BUILD))

STATIC := $(BUILD)/$(NAME).a
SHARED := $(BUILD)/$(NAME).so

SRCS := ctp.c

OBJS := $(addprefix $(BUILD)/, $(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)


all: $(STATIC) $(SHARED)

$(STATIC): $(OBJS)
	ar rcs $@ $^
$(SHARED): $(OBJS)
	$(CC) -shared -o $@ $^

$(BUILD)/%.o: %.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@


clean:
	rm -rf $(DEPS) $(OBJS)

fclean: clean
	rm -f $(STATIC) $(SHARED)

re: fclean all

-include $(DEPS)