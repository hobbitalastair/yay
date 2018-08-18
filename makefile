PREFIX := ${DESTDIR}/usr
BINDIR := ${PREFIX}/bin
LIBS = -lncurses
CC = gcc
CFLAGS := -Wall -Werror -O2 -g -Wno-error=unused-but-set-variable $(CFLAGS)
OBJS = yay

all: $(OBJS)

%: %.c
	$(CC) -o $@ $< $(LIBS) $(CFLAGS)

%: %.sh
	cp -f $< $@
	chmod +x $@

install: $(OBJS)
	mkdir -p "${BINDIR}/"
	for obj in ${OBJS}; do \
	    install -m755 "$$obj" "${BINDIR}/"; \
	done

clean:
	rm -f $(OBJS)
