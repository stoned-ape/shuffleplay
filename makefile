
all: shuffleplay

shuffleplay: main.c makefile
	clang -g main.c -o shuffleplay

run: shuffleplay
	./shuffleplay
