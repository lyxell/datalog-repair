
CXXFLAGS = -std=c++17 -O2 -D__EMBEDDED_SOUFFLE__

all: sjp repair.o program.o

repair.cpp: repair.dl rules/1155.dl
	souffle --no-warn \
			--generate=$@ \
			--fact-dir=build \
			--output-dir=build \
			repair.dl

.PHONY: sjp

sjp:
	$(MAKE) -C sjp

.PHONY: clean

clean:
	rm -rf repair.o program.o
	$(MAKE) -C sjp clean
