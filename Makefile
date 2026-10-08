CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDFLAGS += -pthread

all: bserve bcurl

bserve: bserve.cpp frame.cpp frame.hpp
	$(CXX) $(CXXFLAGS) -o $@ bserve.cpp frame.cpp $(LDFLAGS)

bcurl: bcurl.cpp frame.cpp frame.hpp
	$(CXX) $(CXXFLAGS) -o $@ bcurl.cpp frame.cpp $(LDFLAGS)

test: all
	./tests/run.sh

clean:
	rm -f bserve bcurl

.PHONY: all test clean
