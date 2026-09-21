CXX      ?= clang++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
GPU      ?= 1
CUDA     ?= 0
UNAME    := $(shell uname)

SRC  := lowpoly.cpp
LIBS := -lpthread
NOGPU := 1

ifeq ($(GPU),1)
ifeq ($(UNAME),Darwin)
SRC   += metal.mm
LIBS  += -framework Metal -framework Foundation -framework ImageIO -framework CoreGraphics -framework CoreServices
NOGPU := 0
endif
ifeq ($(CUDA),1)
SRC   += cuda.cu
NOGPU := 0
endif
endif
ifeq ($(NOGPU),1)
CXXFLAGS += -DLOWPOLY_NO_GPU
endif

ifeq ($(CUDA),1)
NVCC    ?= nvcc
COMPILE  = $(NVCC) -O3 -std=c++17 -Xcompiler -O3 -Xcompiler -Wno-unused-parameter
else
COMPILE  = $(CXX) $(CXXFLAGS)
endif

lowpoly: $(SRC) lowpoly.h
	$(COMPILE) -o $@ $(SRC) $(LIBS)

lowpoly-test: $(SRC) lowpoly.h
	$(COMPILE) -DLOWPOLY_TESTS -o $@ $(SRC) $(LIBS)

test: lowpoly-test
	./lowpoly-test

LOOK := --uniform 0.02 --canny-low 20 --canny-high 60
readme-images: lowpoly
	for n in canny-flower flower monarch moto-butterfly; do mkdir -p docs/img/$$n; done; for n in canny-flower flower monarch; do ./lowpoly examples/$$n.jpg --points 2500 $(LOOK) --stages docs/img/$$n -o docs/img/$$n/final.png; done
	./lowpoly examples/moto-butterfly.jpg --points 3000 --uniform 0.02 --canny-low 40 --canny-high 120 --border 64 --stages docs/img/moto-butterfly -o docs/img/moto-butterfly/final.png

clean:
	rm -f lowpoly lowpoly-test

.PHONY: test clean readme-images
