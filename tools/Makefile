# tools/Makefile — Build all Windows XP graphics and hardware verification tools

SUBDIRS = test-gdi test-ddraw test-d3d8 test-d3d9 test-opengl test-modeswitch probe-video eax-test

all:
	@for dir in $(SUBDIRS); do \
		echo "=== Building $$dir ==="; \
		$(MAKE) -C $$dir CC="$(CC)" CFLAGS="$(CFLAGS)" || exit 1; \
	done

clean:
	@for dir in $(SUBDIRS); do \
		echo "=== Cleaning $$dir ==="; \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all clean $(SUBDIRS)
