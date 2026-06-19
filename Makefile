CXXFLAGS = -std=c++23 -g -O2 -fPIC -shared -fno-gnu-unique -Wall -Wextra
HYPRLAND_CFLAGS = $(shell pkg-config --cflags hyprland)

OUT = hyprscrolling-plus.so
SRC = src/main.cpp

all: $(OUT)

$(OUT): $(SRC)
	$(CXX) $(CXXFLAGS) $(HYPRLAND_CFLAGS) $(SRC) -o $(OUT)

clean:
	rm -f $(OUT)
