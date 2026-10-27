#include "bitboard.h"
#include "eval.h"
#include "position.h"
#include "uci.h"

int main(int argc, char* argv[]) {
    Bitboards::init();
    Position::init();
    Eval::init();
    return UCI::loop(argc, argv);
}
