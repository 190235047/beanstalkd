program := beanstalkd
rel_ver := $(shell ./rel-ver.sh)
export CFLAGS := -O2 -Wall -Werror
export LDFLAGS := -levent

sources := $(shell ls *.c | fgrep -v $(program))
objects := $(sources:.c=.o)
tests := $(sources:%=tests/test_%)

all: $(program)

debug: export CFLAGS := -g -pg -Wall -Werror -DDEBUG
debug: export LDFLAGS := -pg -levent
debug: all

check: export CFLAGS := -g -pg -Wall -Werror
check: export LDFLAGS := -pg -levent
check: tests/cutcheck $(objects)
	./tests/cutcheck
	@echo

#ifneq ($(MAKECMDGOALS),clean)
-include $(sources:%.c=.%.d) .$(program).d
#endif

$(program): $(objects) $(program).o

tests/cutcheck.c: $(tests)
	cutgen -o tests/cutcheck.c $(tests)

tests/cutcheck: tests/cutcheck.o $(objects) $(tests:.c=.o)

pkg:
	./pkg.sh $(program) $(rel_ver) $(program)-$(rel_ver).tar.gz

clean:
	rm -f $(program) *.o .*.d tests/*.o tests/cutcheck* core gmon.out

# .DELETE_ON_ERROR:
.PHONY: all debug check pkg clean

# This tells make how to generate dependency files
.%.d: %.c
	@$(SHELL) -ec '$(CC) -MM $(CPPFLAGS) $< \
	              | sed '\''s/\($*\)\.o[ :]*/\1.o $@ : /g'\'' > $@; \
				  [ -s $@ ] || rm -f $@'

