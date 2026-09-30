CXX      ?= clang++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
GPU      ?= 1
CUDA     ?= 0
ifeq ($(OS),Windows_NT)
UNAME    := Windows
EXE      := .exe
else
UNAME    := $(shell uname)
EXE      :=
endif

SRC  := lowpoly.cpp
LIBS := -lpthread
ifeq ($(UNAME),Windows)
LIBS := -lshell32
endif
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
ifeq ($(UNAME),Windows)
COMPILE  = $(NVCC) -O3 -std=c++17 -Xcompiler /O2 -Xcompiler /EHsc -D_CRT_SECURE_NO_WARNINGS
else
COMPILE  = $(NVCC) -O3 -std=c++17 -Xcompiler -O3 -Xcompiler -Wno-unused-parameter
endif
else
COMPILE  = $(CXX) $(CXXFLAGS)
endif

lowpoly$(EXE): $(SRC) lowpoly.h
	$(COMPILE) -o $@ $(SRC) $(LIBS)

lowpoly-test$(EXE): $(SRC) lowpoly.h
	$(COMPILE) -DLOWPOLY_TESTS -o $@ $(SRC) $(LIBS)

ifneq ($(EXE),)
lowpoly: lowpoly$(EXE)
lowpoly-test: lowpoly-test$(EXE)
endif

test: lowpoly-test$(EXE)
	./lowpoly-test$(EXE)

LOOK := --uniform 0.02 --canny-low 20 --canny-high 60
readme-images: lowpoly$(EXE)
	for n in canny-flower flower monarch moto-butterfly; do mkdir -p docs/img/$$n; done; for n in canny-flower flower monarch; do ./lowpoly examples/$$n.jpg --points 2500 $(LOOK) --stages docs/img/$$n -o docs/img/$$n/final.png; done
	./lowpoly examples/moto-butterfly.jpg --points 3000 --uniform 0.02 --canny-low 40 --canny-high 120 --stages docs/img/moto-butterfly -o docs/img/moto-butterfly/final.png

clean:
	rm -f lowpoly lowpoly-test lowpoly.exe lowpoly-test.exe

.PHONY: test clean readme-images
